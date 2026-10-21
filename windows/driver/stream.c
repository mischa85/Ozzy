#include "stream.h"

static enum ozzy_pcm_dir wstream_dir(const struct wstream* s) {
    return s->render ? OZZY_PCM_OUT : OZZY_PCM_IN;
}

static uint64_t wstream_pos(struct wstream* s) {
    return (uint64_t)InterlockedCompareExchange64(&s->pos, 0, 0);
}

/* Frames between the ring position and the device: the engine encodes output this far ahead
 * of the clock (its PCM out transfers in flight) */
static uint32_t wstream_in_flight_frames(const struct wstream* s) {
    return s->render && s->dev->shm ? s->dev->shm->audio.safety_offset_frames : 0;
}

/* Take the ring back from the engine and wait out completions that may still use it */
static void wstream_detach(struct wstream* s) {
    if (!s->attached) return;
    if (s->dev->engine) ozzy_engine_set_pcm_ring(s->dev->engine, wstream_dir(s), NULL);
    kbus_set_stream(&s->dev->bus, wstream_dir(s), NULL);
    kbus_wait_completions(&s->dev->bus);
    s->attached = false;
}

/* The stream no longer holds the device's rate (wdev_stream_rate) */
static void wstream_release_rate(struct wstream* s) {
    if (!s->holding) return;
    WdfWaitLockAcquire(s->dev->lock, NULL);
    s->dev->holding[wstream_dir(s)]--;
    s->holding = false;
    WdfWaitLockRelease(s->dev->lock);
}

/* Also cleans up after a failed allocation (@packets NULL) */
static VOID wstream_free_rt_packets(ACXSTREAM stream, PACX_RTPACKET packets, ULONG count) {
    PAGED_CODE();
    UNREFERENCED_PARAMETER(count);
    struct wstream* s = wstream_get(stream);
    wstream_detach(s);
    for (ULONG i = 0; i < WSTREAM_MAX_PACKETS; i++) {
        if (s->mdl[i]) IoFreeMdl(s->mdl[i]);
        s->mdl[i] = NULL;
    }
    if (s->buffer) ExFreePoolWithTag(s->buffer, OZZY_POOL_TAG);
    s->buffer = NULL;
    s->packet_count = 0;
    if (packets) ExFreePoolWithTag(packets, OZZY_POOL_TAG);
}

/* The ring the engine uses is the packets' data, so it has to be contiguous. Packet sizes from
 * the audio engine are rarely whole pages, so for two packets (the usual case) use one
 * allocation laid out so that packet 0 ends on a page boundary where packet 1 starts: each
 * packet's MDL then covers its own pages, and the data runs on from one to the other. Any
 * other count needs page-sized packets. The pages are zeroed (no kernel data reaches the
 * mapping). */
static NTSTATUS wstream_allocate_rt_packets(ACXSTREAM stream, ULONG count, ULONG size, PACX_RTPACKET* out) {
    PAGED_CODE();
    struct wstream* s = wstream_get(stream);
    if (!count || count > WSTREAM_MAX_PACKETS || !size || size % s->block_align) return STATUS_INVALID_PARAMETER;
    ULONG page_size = ROUND_TO_PAGES(size);
    bool pages = size == page_size;
    if (count != 2 && !pages) return STATUS_NOT_SUPPORTED;
    /* With two packets, packet 0's data starts page_size - size into its page span */
    s->data_offset = pages ? 0 : page_size - size;
    ULONG total = pages ? count * size : 2 * page_size;

    PACX_RTPACKET packets = ExAllocatePool2(POOL_FLAG_NON_PAGED, count * sizeof(ACX_RTPACKET), OZZY_POOL_TAG);
    s->buffer = ExAllocatePool2(POOL_FLAG_NON_PAGED, total, OZZY_POOL_TAG);
    if (!packets || !s->buffer) {
        if (packets) ExFreePoolWithTag(packets, OZZY_POOL_TAG);
        wstream_free_rt_packets(stream, NULL, 0);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (ULONG i = 0; i < count; i++) {
        ACX_RTPACKET_INIT(&packets[i]);
        uint8_t* span = pages ? s->buffer + i * size : s->buffer + i * page_size;
        ULONG span_size = pages ? size : page_size;
        s->mdl[i] = IoAllocateMdl(span, span_size, FALSE, FALSE, NULL);
        if (!s->mdl[i]) {
            ExFreePoolWithTag(packets, OZZY_POOL_TAG);
            wstream_free_rt_packets(stream, NULL, 0);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        MmBuildMdlForNonPagedPool(s->mdl[i]);
        WDF_MEMORY_DESCRIPTOR_INIT_MDL(&packets[i].RtPacketBuffer, s->mdl[i], span_size);
        packets[i].RtPacketSize = size;
        packets[i].RtPacketOffset = i == 0 ? s->data_offset : 0;
    }
    s->packet_count = count;
    s->packet_frames = size / s->block_align;
    kbus_log("[Ozzy] %s stream at %u Hz: %u packets of %u frames", s->render ? "render" : "capture", s->rate,
             (unsigned)count, (unsigned)s->packet_frames);
    *out = packets;
    return STATUS_SUCCESS;
}

/* Hand the ring to the engine, continuing from where the stream stopped: the engine picks it
 * up with its next packet and reports every packet from then on. First the device moves to the
 * stream's rate, which the stream holds from now until it pauses: Windows creates and prepares a
 * stream of every format to try them when the device arrives, but runs none of them. */
static NTSTATUS wstream_run(ACXSTREAM stream) {
    PAGED_CODE();
    struct wstream* s = wstream_get(stream);
    if (!s->dev->engine || !s->buffer) return STATUS_DEVICE_NOT_READY;
    if (!s->holding) {
        NTSTATUS status = wdev_stream_rate(s->dev, s->render, s->rate, true);
        if (!NT_SUCCESS(status)) return status;
        s->holding = true;   /* counted in holding[] by wdev_stream_rate */
    }

    uint64_t pos = wstream_pos(s);
    s->ring.base = s->buffer + s->data_offset;
    s->ring.frames = s->packet_count * s->packet_frames;
    s->ring.channels = s->block_align / 3;
    s->ring.start = pos;
    s->boundary_qpc = (uint64_t)KeQueryPerformanceCounter(NULL).QuadPart;
    InterlockedExchange64(&s->completed, (LONG64)(pos / s->packet_frames));
    s->next_boundary = (pos / s->packet_frames + 1) * s->packet_frames;

    kbus_set_stream(&s->dev->bus, wstream_dir(s), s);
    ozzy_engine_set_pcm_ring(s->dev->engine, wstream_dir(s), &s->ring);
    s->attached = true;
    wsettings_changed(s->dev);   /* "in use" for ozzy-cli and the ozzy app */
    return STATUS_SUCCESS;
}

static NTSTATUS wstream_pause(ACXSTREAM stream) {
    PAGED_CODE();
    struct wstream* s = wstream_get(stream);
    wstream_detach(s);
    wstream_release_rate(s);
    wsettings_changed(s->dev);
    return STATUS_SUCCESS;
}

/* Complete every WaveRT packet the engine has finished (render: read all of it; capture:
 * written all of it) */
void wstream_progress(struct wstream* s, uint64_t frames, uint64_t qpc) {
    InterlockedExchange64(&s->pos, (LONG64)frames);
    InterlockedExchange64(&s->pos_qpc, (LONG64)qpc);
    uint64_t completed = (uint64_t)s->completed;
    for (uint32_t n = ozzy_pcm_boundaries(&s->next_boundary, frames, s->packet_frames); n; n--, completed++) {
        InterlockedExchange64(&s->last_start_qpc, (LONG64)s->boundary_qpc);
        InterlockedExchange64(&s->completed, (LONG64)(completed + 1));
        AcxRtStreamNotifyPacketComplete(s->stream, completed, qpc);
        s->boundary_qpc = qpc;
    }
}

uint32_t wstream_fifo_frames(const struct wstream* s) {
    return wstream_in_flight_frames(s);
}

static NTSTATUS wstream_get_hw_latency(ACXSTREAM stream, ULONG* fifo_size, ULONG* delay) {
    struct wstream* s = wstream_get(stream);
    *fifo_size = wstream_fifo_frames(s) * s->block_align;
    *delay = 0;
    return STATUS_SUCCESS;
}

/* The audio engine wrote @packet. The engine reads it as the device's clock reaches it, so
 * there is nothing to start; a packet behind the one being read came too late. */
static NTSTATUS wstream_set_render_packet(ACXSTREAM stream, ULONG packet, ULONG flags, ULONG eos_length) {
    UNREFERENCED_PARAMETER(flags);
    UNREFERENCED_PARAMETER(eos_length);
    struct wstream* s = wstream_get(stream);
    ULONG current = (ULONG)(wstream_pos(s) / s->packet_frames);
    if (packet < current) return STATUS_DATA_LATE_ERROR;
    return STATUS_SUCCESS;
}

static NTSTATUS wstream_get_current_packet(ACXSTREAM stream, PULONG packet) {
    struct wstream* s = wstream_get(stream);
    *packet = (ULONG)(wstream_pos(s) / s->packet_frames);
    return STATUS_SUCCESS;
}

static NTSTATUS wstream_get_capture_packet(ACXSTREAM stream, ULONG* last_packet, ULONGLONG* qpc_packet_start,
                                           BOOLEAN* more_data) {
    struct wstream* s = wstream_get(stream);
    /* Before the first packet completes this is packet -1, as the ACX sample reports it */
    LONG64 completed = InterlockedCompareExchange64(&s->completed, 0, 0);
    *last_packet = (ULONG)(completed - 1);
    *qpc_packet_start = (ULONGLONG)InterlockedCompareExchange64(&s->last_start_qpc, 0, 0);
    *more_data = FALSE;
    return STATUS_SUCCESS;
}

/* Frames at the device: render is behind the ring position by the packets in flight */
static NTSTATUS wstream_get_presentation_position(ACXSTREAM stream, PULONGLONG position, PULONGLONG qpc) {
    struct wstream* s = wstream_get(stream);
    uint64_t pos = wstream_pos(s);
    uint32_t behind = wstream_in_flight_frames(s);
    *position = pos > behind ? pos - behind : 0;
    *qpc = (ULONGLONG)InterlockedCompareExchange64(&s->pos_qpc, 0, 0);
    return STATUS_SUCCESS;
}

/*
 * KSPROPERTY_AUDIO_POSITION (get): where the device is in the WaveRT buffer, in bytes, for clients
 * that run the pin themselves instead of through the audio engine's packets (PortAudio's WDM-KS,
 * which has no position register from ACX to map). Render: the play offset is the frame at the
 * device, the ring position less the transfers in flight (the client adds those back from the
 * FIFO size, KSPROPERTY_RTAUDIO_HWLATENCY); capture: where the device writes next. Both offsets
 * the same; 0 until the stream has run, which such clients check.
 */
static VOID wstream_evt_position(WDFOBJECT object, WDFREQUEST request) {
    struct wstream* s = wstream_get((ACXSTREAM)object);
    ACX_REQUEST_PARAMETERS params;
    ACX_REQUEST_PARAMETERS_INIT(&params);
    AcxRequestGetParameters(request, &params);

    NTSTATUS status;
    ULONG_PTR size = sizeof(KSAUDIO_POSITION);
    if (params.Parameters.Property.ValueCb == 0) {
        status = STATUS_BUFFER_OVERFLOW;   /* the caller asks for the size */
    } else if (params.Parameters.Property.ValueCb < size || !params.Parameters.Property.Value) {
        status = STATUS_BUFFER_TOO_SMALL;
        size = 0;
    } else {
        KSAUDIO_POSITION* position = (KSAUDIO_POSITION*)params.Parameters.Property.Value;
        uint64_t frames = (uint64_t)s->packet_count * s->packet_frames;
        uint64_t pos = wstream_pos(s);
        uint64_t at = 0;
        if (frames) {
            uint32_t behind = wstream_in_flight_frames(s);
            if (s->render) pos = pos > behind ? pos - behind : 0;
            at = pos % frames * s->block_align;
        }
        position->PlayOffset = at;
        position->WriteOffset = at;
        status = STATUS_SUCCESS;
    }
    WdfRequestCompleteWithInformation(request, status, size);
}

static NTSTATUS wstream_prepare_hardware(ACXSTREAM stream) {
    UNREFERENCED_PARAMETER(stream);
    return STATUS_SUCCESS;
}

static NTSTATUS wstream_release_hardware(ACXSTREAM stream) {
    PAGED_CODE();
    wstream_release_rate(wstream_get(stream));
    return STATUS_SUCCESS;
}

static VOID wstream_destroy(WDFOBJECT object) {
    struct wstream* s = wstream_get((ACXSTREAM)object);
    wstream_detach(s);
    if (s->dev) wstream_release_rate(s);
    if (s->dev && s->dev->streams[wstream_dir(s)] == s) s->dev->streams[wstream_dir(s)] = NULL;
}

NTSTATUS wstream_evt_create(WDFDEVICE device, ACXCIRCUIT circuit, ACXPIN pin, PACXSTREAM_INIT init,
                            ACXDATAFORMAT format, const GUID* signal_processing_mode, ACXOBJECTBAG var_arguments) {
    UNREFERENCED_PARAMETER(pin);
    UNREFERENCED_PARAMETER(signal_processing_mode);
    UNREFERENCED_PARAMETER(var_arguments);
    PAGED_CODE();
    bool render = wcircuit_get(circuit)->render;
    struct wdev* dev = wdev_get(device);

    /* A rate the device can take now; it moves to it once the stream is prepared */
    uint32_t rate = AcxDataFormatGetSampleRate(format);
    NTSTATUS status = wdev_stream_rate(dev, render, rate, false);
    if (!NT_SUCCESS(status)) return status;

    ACX_STREAM_CALLBACKS callbacks;
    ACX_STREAM_CALLBACKS_INIT(&callbacks);
    callbacks.EvtAcxStreamPrepareHardware = wstream_prepare_hardware;
    callbacks.EvtAcxStreamReleaseHardware = wstream_release_hardware;
    callbacks.EvtAcxStreamRun = wstream_run;
    callbacks.EvtAcxStreamPause = wstream_pause;
    status = AcxStreamInitAssignAcxStreamCallbacks(init, &callbacks);
    if (!NT_SUCCESS(status)) return status;

    ACX_RT_STREAM_CALLBACKS rt;
    ACX_RT_STREAM_CALLBACKS_INIT(&rt);
    rt.EvtAcxStreamGetHwLatency = wstream_get_hw_latency;
    rt.EvtAcxStreamAllocateRtPackets = wstream_allocate_rt_packets;
    rt.EvtAcxStreamFreeRtPackets = wstream_free_rt_packets;
    if (render) rt.EvtAcxStreamSetRenderPacket = wstream_set_render_packet;
    else rt.EvtAcxStreamGetCapturePacket = wstream_get_capture_packet;
    rt.EvtAcxStreamGetCurrentPacket = wstream_get_current_packet;
    rt.EvtAcxStreamGetPresentationPosition = wstream_get_presentation_position;
    status = AcxStreamInitAssignAcxRtStreamCallbacks(init, &rt);
    if (!NT_SUCCESS(status)) return status;
    AcxStreamInitSetAcxRtStreamSupportsNotifications(init);
    /* Without it the stream still runs for the audio engine: no failure */
    static ACX_PROPERTY_ITEM properties[] = {
        {
            .Set = &KSPROPSETID_Audio,
            .Id = KSPROPERTY_AUDIO_POSITION,
            .Flags = ACX_PROPERTY_ITEM_FLAG_GET,
            .EvtAcxObjectProcessRequest = wstream_evt_position,
            .ValueCb = sizeof(KSAUDIO_POSITION),
        },
    };
    NTSTATUS assigned = AcxStreamInitAssignProperties(init, properties, ARRAYSIZE(properties));
    if (!NT_SUCCESS(assigned)) kbus_log("[Ozzy] stream position property: 0x%08x", assigned);

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, wstream);
    attributes.EvtDestroyCallback = wstream_destroy;
    ACXSTREAM stream;
    status = AcxRtStreamCreate(device, circuit, &attributes, &init, &stream);
    if (!NT_SUCCESS(status)) return status;

    /* The context is zeroed */
    struct wstream* s = wstream_get(stream);
    s->stream = stream;
    s->dev = dev;
    s->render = render;
    s->block_align = AcxDataFormatGetBlockAlign(format);
    s->rate = rate;
    dev->streams[wstream_dir(s)] = s;
    return STATUS_SUCCESS;
}
