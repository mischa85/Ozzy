#include "midi.h"

/* From Windows MIDI Services' MidiKSDef.h (the facts: GUIDs, ids and layouts). Built for the
 * 64-bit client the MIDI service is. */
static const GUID ksdataformat_subtype_ump = { 0xfbffd49e, 0xce26, 0x464a, { 0x9d, 0xfc, 0xfe, 0xe4, 0x24, 0x56, 0xc8, 0x1c } };
static const GUID kspropsetid_midi_looped = { 0x1f306ba6, 0xfd9b, 0x427a, { 0xbc, 0xb3, 0x27, 0xcb, 0xcf, 0x0e, 0x0f, 0x19 } };

enum wmidi_looped_property {
    WMIDI_LOOPED_BUFFER = 0,               /* get, control: requested size; value: struct wmidi_looped_buffer */
    WMIDI_LOOPED_REGISTERS = 1,            /* get, value: struct wmidi_looped_registers */
    WMIDI_LOOPED_NOTIFICATION_EVENT = 2,   /* set, value: struct wmidi_looped_events */
};

struct wmidi_looped_buffer {               /* KSMIDILOOPED_BUFFER */
    PVOID address;
    ULONG size;
};

struct wmidi_looped_registers {            /* KSMIDILOOPED_REGISTERS */
    PVOID write_position;
    PVOID read_position;
};

struct wmidi_looped_events {               /* KSMIDILOOPED_EVENT2 */
    HANDLE write_event;
    HANDLE read_event;
};

static const GUID kspropsetid_midi2_endpoint_information = { 0x0814552d, 0x2a1e, 0x48fe, { 0x9f, 0xbd, 0x70, 0xac, 0x67, 0x91, 0x07, 0x55 } };

/* What the KS transport asks every pin for. Without a native format it treats the endpoint as
 * MIDI 2.0 and makes no MIDI 1.0 ports; the rest it can do without. */
enum wmidi_info_property {
    WMIDI_INFO_NATIVE_DATA_FORMAT = 0,     /* GUID: KSDATAFORMAT_SUBTYPE_MIDI, a MIDI 1.0 device */
    WMIDI_INFO_GROUP_TERMINAL_BLOCKS = 1,  /* KSMULTIPLE_ITEM + struct wmidi_gtb each: the ports */
    WMIDI_INFO_DEVICE_VID = 4,             /* USHORT */
    WMIDI_INFO_DEVICE_PID = 5,             /* USHORT */
};

/* A group terminal block (UMP_GROUP_TERMINAL_BLOCK_HEADER), here with an empty name: the
 * service names the port after the device */
#pragma pack(push, 1)
struct wmidi_gtb {
    USHORT size;                           /* this, name included */
    UCHAR number;
    UCHAR direction;                       /* 1: into the device (a MIDI out port), 2: out of it */
    UCHAR first_group;
    UCHAR group_count;
    UCHAR protocol;                        /* 1: MIDI 1.0, up to 64-bit packets */
    USHORT max_input_bandwidth;            /* 0: unknown */
    USHORT max_output_bandwidth;
    WCHAR name[1];
};
#pragma pack(pop)

/* Each message in a ring: this header (UMPDATAFORMAT, 16 bytes), then ByteCount bytes of UMP */
struct wmidi_message_header {
    LONGLONG position;                     /* the time: QPC ticks */
    ULONG byte_count;                      /* 4 to 16 */
};
#define WMIDI_HEADER_SIZE 16

#define WMIDI_MAX_BUFFER (256 * PAGE_SIZE)

/* The circuit's pins: the streaming pins carry the UMP, the bridges stand for the device */
enum wmidi_pin { WMIDI_PIN_OUT = 0, WMIDI_PIN_OUT_BRIDGE = 1, WMIDI_PIN_IN = 2, WMIDI_PIN_IN_BRIDGE = 3 };

static const GUID midi_component = { 0x2f6a1c3e, 0x8b4d, 0x4e57, { 0x9c, 0x21, 0x5a, 0x7e, 0x3d, 0x90, 0x6b, 0x14 } };

/* KSDATAFORMAT for UMP (ACX keeps a pointer to it) */
static KSDATAFORMAT midi_format = {
    sizeof(KSDATAFORMAT), 0, 0, 0,
    /* KSDATAFORMAT_TYPE_MUSIC, the UMP subtype, KSDATAFORMAT_SPECIFIER_NONE */
    { 0xe725d360, 0x62cc, 0x11cf, { 0xa5, 0xd6, 0x28, 0xdb, 0x04, 0xc1, 0x00, 0x00 } },
    { 0xfbffd49e, 0xce26, 0x464a, { 0x9d, 0xfc, 0xfe, 0xe4, 0x24, 0x56, 0xc8, 0x1c } },
    { 0x0f6417d6, 0xc318, 0x11d0, { 0xa4, 0x3f, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96 } },
};

/* --- The rings --- */

/* Bytes the reader can take, and the room the writer has (one byte always stays free) */
static ULONG wmidi_used(ULONG read, ULONG write, ULONG size) {
    return (write + size - read) % size;
}

static void wmidi_copy_in(struct wmidi_ring* r, ULONG at, const void* src, ULONG n) {
    ULONG first = n < r->size - at ? n : r->size - at;
    RtlCopyMemory(r->buffer + at, src, first);
    RtlCopyMemory(r->buffer, (const uint8_t*)src + first, n - first);
}

static void wmidi_copy_out(const struct wmidi_ring* r, ULONG at, void* dst, ULONG n) {
    ULONG first = n < r->size - at ? n : r->size - at;
    RtlCopyMemory(dst, r->buffer + at, first);
    RtlCopyMemory((uint8_t*)dst + first, r->buffer, n - first);
}

/* Up to DISPATCH_LEVEL: one UMP message of @count words into an in ring, unless it is full
 * (then it is dropped: the completion path doesn't wait) */
static void wmidi_put(struct wmidi_ring* r, const uint32_t* words, unsigned int count) {
    ULONG need = WMIDI_HEADER_SIZE + count * 4;
    ULONG read = (ULONG)InterlockedCompareExchange(&r->registers[0], 0, 0);
    ULONG write = (ULONG)r->registers[1];
    if (read >= r->size || write >= r->size || r->size - 1 - wmidi_used(read, write, r->size) < need) return;

    uint8_t header[WMIDI_HEADER_SIZE] = { 0 };
    struct wmidi_message_header h = { KeQueryPerformanceCounter(NULL).QuadPart, count * 4 };
    RtlCopyMemory(header, &h, sizeof(h) < WMIDI_HEADER_SIZE ? sizeof(h) : WMIDI_HEADER_SIZE);
    wmidi_copy_in(r, write, header, WMIDI_HEADER_SIZE);
    wmidi_copy_in(r, (write + WMIDI_HEADER_SIZE) % r->size, words, count * 4);
    InterlockedExchange(&r->registers[1], (LONG)((write + need) % r->size));
    KeSetEvent(r->write_event, IO_NO_INCREMENT, FALSE);
}

void wmidi_service(void* ctx) {
    struct wdev* dev = ctx;
    if (!dev->shm || InterlockedCompareExchange(&dev->midi_busy, 1, 0)) return;   /* one at a time */

    struct wmidi_stream* s = InterlockedCompareExchangePointer((PVOID volatile*)&dev->midi_in, NULL, NULL);
    uint8_t bytes[64];
    uint32_t n;
    while ((n = ozzy_midi_ring_pop(&dev->shm->midi_in, bytes, sizeof(bytes))) > 0) {
        for (uint32_t i = 0; s && i < n; i++) {
            uint32_t words[MIDI_UMP_ENCODE_MAX_WORDS];
            unsigned int w = midi_ump_encode_byte(&s->encoder, 0, bytes[i], words);
            for (unsigned int k = 0; k < w; k += midi_ump_words(words[k])) wmidi_put(&s->ring, words + k, midi_ump_words(words[k]));
        }
    }
    InterlockedExchange(&dev->midi_busy, 0);
}

/* PASSIVE_LEVEL, the worker: the client's messages, as MIDI 1.0 bytes, toward midi_out. A
 * malformed message (or position) drops everything the client has written. */
static void wmidi_drain_out(struct wmidi_stream* s) {
    struct wmidi_ring* r = &s->ring;
    struct wdev* dev = s->dev;
    ULONG read = (ULONG)r->registers[0];
    ULONG write = (ULONG)InterlockedCompareExchange(&r->registers[1], 0, 0);
    bool moved = false;

    if (!dev->shm || read >= r->size || write >= r->size) {
        if (write < r->size) InterlockedExchange(&r->registers[0], (LONG)write);
        return;
    }
    while (wmidi_used(read, write, r->size) >= WMIDI_HEADER_SIZE) {
        struct wmidi_message_header h;
        uint32_t words[4];
        wmidi_copy_out(r, read, &h, sizeof(h));
        if (h.byte_count < 4 || h.byte_count > 16 || h.byte_count % 4) {
            read = write;
            break;
        }
        if (wmidi_used(read, write, r->size) < WMIDI_HEADER_SIZE + h.byte_count) break;   /* not all there yet */
        wmidi_copy_out(r, (read + WMIDI_HEADER_SIZE) % r->size, words, h.byte_count);
        read = (read + WMIDI_HEADER_SIZE + h.byte_count) % r->size;

        for (ULONG k = 0; k < h.byte_count / 4; k += midi_ump_words(words[k])) {
            uint8_t bytes[8];
            if (k + midi_ump_words(words[k]) > h.byte_count / 4) break;
            unsigned int n = midi_ump_decode(words + k, bytes);
            if (n) moved |= ozzy_midi_queue_write(&dev->shm->midi_out, &s->queue, bytes, n) != 0;
        }
    }
    InterlockedExchange(&r->registers[0], (LONG)read);
    if (ozzy_midi_queue_pending(&s->queue)) moved |= ozzy_midi_queue_write(&dev->shm->midi_out, &s->queue, NULL, 0) != 0;
    if (moved && dev->engine) ozzy_engine_midi_out_pending(dev->engine);
    KeSetEvent(r->read_event, IO_NO_INCREMENT, FALSE);
}

static VOID wmidi_out_thread(PVOID ctx) {
    struct wmidi_stream* s = ctx;
    PVOID objects[2] = { &s->stop, s->ring.write_event };
    LARGE_INTEGER retry;
    retry.QuadPart = -10000;   /* 1 ms: bytes wait in the queue for room in midi_out */

    for (;;) {
        NTSTATUS status = KeWaitForMultipleObjects(2, objects, WaitAny, Executive, KernelMode, FALSE,
                                                   ozzy_midi_queue_pending(&s->queue) ? &retry : NULL, NULL);
        if (status == STATUS_WAIT_0) break;
        wmidi_drain_out(s);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* --- Mapping into the client --- */

/* In the client's context: the buffer's pages twice, back to back, so a message that runs past
 * the end reads on from the start. Reserve the range, release it, and map into it; another
 * thread of the client can take the range in between, so try a few times. */
static NTSTATUS wmidi_map_buffer(struct wmidi_ring* r) {
    for (int attempt = 0; attempt < 8; attempt++) {
        PVOID base = NULL;
        SIZE_T size = 2 * (SIZE_T)r->size;
        if (!NT_SUCCESS(ZwAllocateVirtualMemory(NtCurrentProcess(), &base, 0, &size, MEM_RESERVE, PAGE_NOACCESS)))
            return STATUS_INSUFFICIENT_RESOURCES;
        size = 0;
        ZwFreeVirtualMemory(NtCurrentProcess(), &base, &size, MEM_RELEASE);

        PVOID first = NULL, second = NULL;
        __try {
            first = MmMapLockedPagesSpecifyCache(r->mdl[0], UserMode, MmCached, base, FALSE,
                                                 NormalPagePriority | MdlMappingNoExecute);
            if (first == base)
                second = MmMapLockedPagesSpecifyCache(r->mdl[1], UserMode, MmCached, (uint8_t*)base + r->size, FALSE,
                                                      NormalPagePriority | MdlMappingNoExecute);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (first == base && second == (uint8_t*)base + r->size) {
            r->user = base;
            return STATUS_SUCCESS;
        }
        if (second) MmUnmapLockedPages(second, r->mdl[1]);
        if (first) MmUnmapLockedPages(first, r->mdl[0]);
    }
    return STATUS_INSUFFICIENT_RESOURCES;
}

/* Any context: undo everything the properties set up (in the client's context for the views) */
static void wmidi_ring_free(struct wmidi_ring* r) {
    if (r->process && (r->user || r->registers_user)) {
        KAPC_STATE apc;
        KeStackAttachProcess(r->process, &apc);
        if (r->user) {
            MmUnmapLockedPages((uint8_t*)r->user + r->size, r->mdl[1]);
            MmUnmapLockedPages(r->user, r->mdl[0]);
        }
        if (r->registers_user) MmUnmapLockedPages(r->registers_user, r->registers_mdl);
        KeUnstackDetachProcess(&apc);
    }
    for (int i = 0; i < 2; i++) {
        if (r->mdl[i]) IoFreeMdl(r->mdl[i]);
    }
    if (r->registers_mdl) IoFreeMdl(r->registers_mdl);
    if (r->buffer) ExFreePoolWithTag(r->buffer, OZZY_POOL_TAG);
    if (r->registers) ExFreePoolWithTag((PVOID)r->registers, OZZY_POOL_TAG);
    if (r->write_event) ObDereferenceObject(r->write_event);
    if (r->read_event) ObDereferenceObject(r->read_event);
    if (r->process) ObDereferenceObject(r->process);
    RtlZeroMemory(r, sizeof(*r));
}

/* --- KSPROPSETID_MidiLoopedStreaming on the stream --- */

/* Run @fn in the requesting process's context; the views and handles are that process's */
static NTSTATUS wmidi_in_requestor(WDFREQUEST request, struct wmidi_ring* r,
                                   NTSTATUS (*fn)(struct wmidi_ring*, ACX_REQUEST_PARAMETERS*, ULONG_PTR*),
                                   ACX_REQUEST_PARAMETERS* params, ULONG_PTR* info) {
    PEPROCESS process = IoGetRequestorProcess(WdfRequestWdmGetIrp(request));
    if (!process) return STATUS_INVALID_DEVICE_REQUEST;
    if (r->process && r->process != process) return STATUS_INVALID_DEVICE_REQUEST;   /* one client per stream */

    KAPC_STATE apc;
    bool attach = process != PsGetCurrentProcess();
    if (attach) KeStackAttachProcess(process, &apc);
    if (!r->process) {
        ObReferenceObject(process);
        r->process = process;
    }
    NTSTATUS status = fn(r, params, info);
    if (attach) KeUnstackDetachProcess(&apc);
    return status;
}

static NTSTATUS wmidi_get_buffer(struct wmidi_ring* r, ACX_REQUEST_PARAMETERS* params, ULONG_PTR* info) {
    if (r->buffer) return STATUS_ALREADY_INITIALIZED;
    if (params->Parameters.Property.ControlCb < sizeof(ULONG) || !params->Parameters.Property.Control)
        return STATUS_INVALID_PARAMETER;
    ULONG requested = *(const ULONG*)params->Parameters.Property.Control;
    if (requested < 8 || requested > WMIDI_MAX_BUFFER) return STATUS_INVALID_PARAMETER;

    r->size = (ULONG)ROUND_TO_PAGES(requested);
    r->buffer = ExAllocatePool2(POOL_FLAG_NON_PAGED, r->size, OZZY_POOL_TAG);
    if (!r->buffer) return STATUS_INSUFFICIENT_RESOURCES;
    for (int i = 0; i < 2; i++) {
        r->mdl[i] = IoAllocateMdl(r->buffer, r->size, FALSE, FALSE, NULL);
        if (!r->mdl[i]) return STATUS_INSUFFICIENT_RESOURCES;
        MmBuildMdlForNonPagedPool(r->mdl[i]);
    }
    NTSTATUS status = wmidi_map_buffer(r);
    if (!NT_SUCCESS(status)) return status;

    struct wmidi_looped_buffer* out = params->Parameters.Property.Value;
    out->address = r->user;
    out->size = r->size;
    *info = sizeof(*out);
    return STATUS_SUCCESS;
}

/* A page of their own, so the client sees no other kernel memory */
static NTSTATUS wmidi_get_registers(struct wmidi_ring* r, ACX_REQUEST_PARAMETERS* params, ULONG_PTR* info) {
    if (r->registers) return STATUS_ALREADY_INITIALIZED;
    r->registers = ExAllocatePool2(POOL_FLAG_NON_PAGED, PAGE_SIZE, OZZY_POOL_TAG);
    if (!r->registers) return STATUS_INSUFFICIENT_RESOURCES;
    r->registers_mdl = IoAllocateMdl((PVOID)r->registers, PAGE_SIZE, FALSE, FALSE, NULL);
    if (!r->registers_mdl) return STATUS_INSUFFICIENT_RESOURCES;
    MmBuildMdlForNonPagedPool(r->registers_mdl);
    __try {
        r->registers_user = MmMapLockedPagesSpecifyCache(r->registers_mdl, UserMode, MmCached, NULL, FALSE,
                                                         NormalPagePriority | MdlMappingNoExecute);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r->registers_user = NULL;
    }
    if (!r->registers_user) return STATUS_INSUFFICIENT_RESOURCES;

    struct wmidi_looped_registers* out = params->Parameters.Property.Value;
    out->read_position = r->registers_user;
    out->write_position = (uint8_t*)r->registers_user + sizeof(ULONG);
    *info = sizeof(*out);
    return STATUS_SUCCESS;
}

static NTSTATUS wmidi_set_events(struct wmidi_ring* r, ACX_REQUEST_PARAMETERS* params, ULONG_PTR* info) {
    UNREFERENCED_PARAMETER(info);
    if (r->write_event) return STATUS_ALREADY_INITIALIZED;
    const struct wmidi_looped_events* in = params->Parameters.Property.Value;
    NTSTATUS status = ObReferenceObjectByHandle(in->write_event, EVENT_MODIFY_STATE, *ExEventObjectType, UserMode,
                                                (PVOID*)&r->write_event, NULL);
    if (NT_SUCCESS(status))
        status = ObReferenceObjectByHandle(in->read_event, EVENT_MODIFY_STATE, *ExEventObjectType, UserMode,
                                           (PVOID*)&r->read_event, NULL);
    if (!NT_SUCCESS(status) && r->write_event) {
        ObDereferenceObject(r->write_event);
        r->write_event = NULL;
    }
    return status;
}

static void wmidi_property(WDFOBJECT object, WDFREQUEST request,
                           NTSTATUS (*fn)(struct wmidi_ring*, ACX_REQUEST_PARAMETERS*, ULONG_PTR*), ULONG value_cb) {
    PAGED_CODE();
    struct wmidi_stream* s = wmidi_stream_get((ACXSTREAM)object);
    ACX_REQUEST_PARAMETERS params;
    ACX_REQUEST_PARAMETERS_INIT(&params);
    AcxRequestGetParameters(request, &params);
    ULONG_PTR info = 0;
    NTSTATUS status = STATUS_BUFFER_TOO_SMALL;
    if (params.Parameters.Property.ValueCb >= value_cb && params.Parameters.Property.Value)
        status = wmidi_in_requestor(request, &s->ring, fn, &params, &info);
    WdfRequestCompleteWithInformation(request, status, info);
}

static VOID wmidi_evt_buffer(WDFOBJECT object, WDFREQUEST request) {
    wmidi_property(object, request, wmidi_get_buffer, sizeof(struct wmidi_looped_buffer));
}

static VOID wmidi_evt_registers(WDFOBJECT object, WDFREQUEST request) {
    wmidi_property(object, request, wmidi_get_registers, sizeof(struct wmidi_looped_registers));
}

static VOID wmidi_evt_events(WDFOBJECT object, WDFREQUEST request) {
    wmidi_property(object, request, wmidi_set_events, sizeof(struct wmidi_looped_events));
}

/* KSPROPSETID_MIDI2_ENDPOINT_INFORMATION: one MIDI 1.0 port each way, on group 1 */
static VOID wmidi_evt_info(WDFOBJECT object, WDFREQUEST request) {
    PAGED_CODE();
    struct wmidi_stream* s = wmidi_stream_get((ACXSTREAM)object);
    ACX_REQUEST_PARAMETERS params;
    ACX_REQUEST_PARAMETERS_INIT(&params);
    AcxRequestGetParameters(request, &params);

    union {
        GUID format;
        USHORT id;
        struct {
            KSMULTIPLE_ITEM header;
            struct wmidi_gtb blocks[2];
        } gtbs;
    } value;
    RtlZeroMemory(&value, sizeof(value));
    ULONG size;
    switch (params.Parameters.Property.Id) {
    case WMIDI_INFO_NATIVE_DATA_FORMAT:
        value.format = KSDATAFORMAT_SUBTYPE_MIDI;
        size = sizeof(value.format);
        break;
    case WMIDI_INFO_GROUP_TERMINAL_BLOCKS:
        size = sizeof(value.gtbs.header) + sizeof(value.gtbs.blocks);   /* not sizeof(value.gtbs): no padding at the end */
        value.gtbs.header.Size = size;
        value.gtbs.header.Count = 2;
        for (UCHAR i = 0; i < 2; i++) {
            value.gtbs.blocks[i].size = sizeof(struct wmidi_gtb);
            value.gtbs.blocks[i].number = i;
            value.gtbs.blocks[i].direction = i + 1;
            value.gtbs.blocks[i].group_count = 1;
            value.gtbs.blocks[i].protocol = 1;
        }
        break;
    case WMIDI_INFO_DEVICE_VID:
        value.id = s->dev->model->vid;
        size = sizeof(value.id);
        break;
    case WMIDI_INFO_DEVICE_PID:
        value.id = s->dev->model->pid;
        size = sizeof(value.id);
        break;
    default:
        WdfRequestCompleteWithInformation(request, STATUS_NOT_SUPPORTED, 0);
        return;
    }

    NTSTATUS status = STATUS_SUCCESS;
    ULONG_PTR info = size;
    if (params.Parameters.Property.ValueCb == 0) {
        status = STATUS_BUFFER_OVERFLOW;   /* the caller asks for the size */
    } else if (params.Parameters.Property.ValueCb < size || !params.Parameters.Property.Value) {
        status = STATUS_BUFFER_TOO_SMALL;
        info = 0;
    } else {
        RtlCopyMemory(params.Parameters.Property.Value, &value, size);
    }
    WdfRequestCompleteWithInformation(request, status, info);
}

/* --- The stream --- */

static NTSTATUS wmidi_evt_prepare_hardware(ACXSTREAM stream) {
    UNREFERENCED_PARAMETER(stream);
    return STATUS_SUCCESS;
}

static NTSTATUS wmidi_evt_release_hardware(ACXSTREAM stream) {
    UNREFERENCED_PARAMETER(stream);
    return STATUS_SUCCESS;
}

/* In: published to wmidi_service. Out: the worker starts. Both need the ring set up. */
static NTSTATUS wmidi_evt_run(ACXSTREAM stream) {
    PAGED_CODE();
    struct wmidi_stream* s = wmidi_stream_get(stream);
    if (!s->ring.buffer || !s->ring.registers || !s->ring.write_event) return STATUS_INVALID_DEVICE_STATE;

    if (s->in) {
        RtlZeroMemory(&s->encoder, sizeof(s->encoder));
        InterlockedExchangePointer((PVOID volatile*)&s->dev->midi_in, s);
        return STATUS_SUCCESS;
    }
    if (s->thread) return STATUS_SUCCESS;
    KeClearEvent(&s->stop);
    HANDLE handle;
    NTSTATUS status = PsCreateSystemThread(&handle, THREAD_ALL_ACCESS, NULL, NULL, NULL, wmidi_out_thread, s);
    if (!NT_SUCCESS(status)) return status;
    status = ObReferenceObjectByHandle(handle, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID*)&s->thread, NULL);
    ZwClose(handle);
    if (NT_SUCCESS(status)) KeSetPriorityThread(s->thread, LOW_REALTIME_PRIORITY);
    return status;
}

/* Undo Run; also from cleanup */
static void wmidi_stop(struct wmidi_stream* s) {
    if (s->in) {
        if (InterlockedCompareExchangePointer((PVOID volatile*)&s->dev->midi_in, NULL, s) == s)
            kbus_wait_completions(&s->dev->bus);
        return;
    }
    if (!s->thread) return;
    KeSetEvent(&s->stop, IO_NO_INCREMENT, FALSE);
    KeWaitForSingleObject(s->thread, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(s->thread);
    s->thread = NULL;
}

static NTSTATUS wmidi_evt_pause(ACXSTREAM stream) {
    PAGED_CODE();
    wmidi_stop(wmidi_stream_get(stream));
    return STATUS_SUCCESS;
}

static VOID wmidi_evt_cleanup(WDFOBJECT object) {
    struct wmidi_stream* s = wmidi_stream_get((ACXSTREAM)object);
    wmidi_stop(s);
    wmidi_ring_free(&s->ring);
    struct wmidi_stream* volatile* slot = s->in ? &s->dev->midi_streams[1] : &s->dev->midi_streams[0];
    InterlockedCompareExchangePointer((PVOID volatile*)slot, NULL, s);
}

/* A stream on a streaming pin, one per pin */
static NTSTATUS wmidi_evt_create_stream(WDFDEVICE device, ACXCIRCUIT circuit, ACXPIN pin, PACXSTREAM_INIT init,
                                        ACXDATAFORMAT format, const GUID* signal_processing_mode,
                                        ACXOBJECTBAG var_arguments) {
    UNREFERENCED_PARAMETER(format);
    UNREFERENCED_PARAMETER(signal_processing_mode);
    UNREFERENCED_PARAMETER(var_arguments);
    PAGED_CODE();
    struct wdev* dev = wdev_get(device);
    ULONG id = AcxPinGetId(pin);
    if (id != WMIDI_PIN_OUT && id != WMIDI_PIN_IN) return STATUS_NOT_SUPPORTED;
    bool in = id == WMIDI_PIN_IN;
    if (dev->midi_streams[in ? 1 : 0]) return STATUS_INSUFFICIENT_RESOURCES;

    static ACX_PROPERTY_ITEM properties[] = {
        {
            .Set = &kspropsetid_midi_looped,
            .Id = WMIDI_LOOPED_BUFFER,
            .Flags = ACX_PROPERTY_ITEM_FLAG_GET,
            .EvtAcxObjectProcessRequest = wmidi_evt_buffer,
            .ControlCb = sizeof(ULONG),
            .ValueCb = sizeof(struct wmidi_looped_buffer),
        },
        {
            .Set = &kspropsetid_midi_looped,
            .Id = WMIDI_LOOPED_REGISTERS,
            .Flags = ACX_PROPERTY_ITEM_FLAG_GET,
            .EvtAcxObjectProcessRequest = wmidi_evt_registers,
            .ValueCb = sizeof(struct wmidi_looped_registers),
        },
        {
            .Set = &kspropsetid_midi_looped,
            .Id = WMIDI_LOOPED_NOTIFICATION_EVENT,
            .Flags = ACX_PROPERTY_ITEM_FLAG_SET,
            .EvtAcxObjectProcessRequest = wmidi_evt_events,
            .ValueCb = sizeof(struct wmidi_looped_events),
        },
#define WMIDI_INFO_ITEM(id_) \
        { .Set = &kspropsetid_midi2_endpoint_information, .Id = (id_), .Flags = ACX_PROPERTY_ITEM_FLAG_GET, \
          .EvtAcxObjectProcessRequest = wmidi_evt_info }
        WMIDI_INFO_ITEM(WMIDI_INFO_NATIVE_DATA_FORMAT),
        WMIDI_INFO_ITEM(WMIDI_INFO_GROUP_TERMINAL_BLOCKS),
        WMIDI_INFO_ITEM(WMIDI_INFO_DEVICE_VID),
        WMIDI_INFO_ITEM(WMIDI_INFO_DEVICE_PID),
#undef WMIDI_INFO_ITEM
    };
    NTSTATUS status = AcxStreamInitAssignProperties(init, properties, ARRAYSIZE(properties));
    if (!NT_SUCCESS(status)) return status;

    ACX_STREAM_CALLBACKS callbacks;
    ACX_STREAM_CALLBACKS_INIT(&callbacks);
    callbacks.EvtAcxStreamPrepareHardware = wmidi_evt_prepare_hardware;
    callbacks.EvtAcxStreamReleaseHardware = wmidi_evt_release_hardware;
    callbacks.EvtAcxStreamRun = wmidi_evt_run;
    callbacks.EvtAcxStreamPause = wmidi_evt_pause;
    status = AcxStreamInitAssignAcxStreamCallbacks(init, &callbacks);
    if (!NT_SUCCESS(status)) return status;

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, wmidi_stream);
    attributes.EvtCleanupCallback = wmidi_evt_cleanup;
    ACXSTREAM stream;
    status = AcxStreamCreate(device, circuit, &attributes, &init, &stream);
    if (!NT_SUCCESS(status)) return status;

    /* The context is zeroed */
    struct wmidi_stream* s = wmidi_stream_get(stream);
    s->dev = dev;
    s->in = in;
    KeInitializeEvent(&s->stop, NotificationEvent, FALSE);
    s->queue.buf = s->queue_buf;
    s->queue.mask = sizeof(s->queue_buf) - 1;
    dev->midi_streams[in ? 1 : 0] = s;
    return STATUS_SUCCESS;
}

/* --- The circuit --- */

/* KSPROPERTY_PIN_DATARANGES of the streaming pins: one KSDATARANGE_MUSIC for UMP. The rest of
 * the circuit's requests go on to ACX. */
static VOID wmidi_evt_dataranges(ACXOBJECT object, ACXCONTEXT context, WDFREQUEST request) {
    UNREFERENCED_PARAMETER(context);
    PAGED_CODE();
    ACX_REQUEST_PARAMETERS params;
    ACX_REQUEST_PARAMETERS_INIT(&params);
    AcxRequestGetParameters(request, &params);
    if (params.Type != AcxRequestTypeProperty || params.Parameters.Property.ItemType != AcxItemTypePin ||
        (params.Parameters.Property.ItemId != WMIDI_PIN_OUT && params.Parameters.Property.ItemId != WMIDI_PIN_IN)) {
        (VOID)AcxCircuitDispatchAcxRequest((ACXCIRCUIT)object, request);
        return;
    }

    struct {
        KSMULTIPLE_ITEM header;
        KSDATARANGE_MUSIC range;
    } ranges;
    RtlZeroMemory(&ranges, sizeof(ranges));
    ranges.header.Size = sizeof(ranges);
    ranges.header.Count = 1;
    ranges.range.DataRange.FormatSize = sizeof(KSDATARANGE_MUSIC);
    ranges.range.DataRange.MajorFormat = midi_format.MajorFormat;
    ranges.range.DataRange.SubFormat = ksdataformat_subtype_ump;
    ranges.range.DataRange.Specifier = midi_format.Specifier;
    ranges.range.Technology = KSMUSIC_TECHNOLOGY_PORT;
    ranges.range.ChannelMask = 0xFFFF;

    ULONG_PTR info = sizeof(ranges);
    NTSTATUS status;
    if (params.Parameters.Property.ValueCb == 0) {
        status = STATUS_BUFFER_OVERFLOW;   /* the caller asks for the size */
    } else if (params.Parameters.Property.ValueCb < sizeof(ranges) || !params.Parameters.Property.Value) {
        status = STATUS_BUFFER_TOO_SMALL;
        info = 0;
    } else {
        RtlCopyMemory(params.Parameters.Property.Value, &ranges, sizeof(ranges));
        status = STATUS_SUCCESS;
    }
    WdfRequestCompleteWithInformation(request, status, info);
}

static NTSTATUS wmidi_pin(ACXCIRCUIT circuit, ACX_PIN_TYPE type, ACX_PIN_COMMUNICATION communication, bool bridge,
                          ACXPIN* pin) {
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = circuit;
    ACX_PIN_CONFIG config;
    ACX_PIN_CONFIG_INIT(&config);
    config.Type = type;
    config.Communication = communication;
    config.Category = &KSCATEGORY_AUDIO;
    if (bridge) config.Qualifier = AcxPinQualifierBridgeDevice;
    else config.Name = &KSAUDFNAME_MIDI;
    return AcxPinCreate(circuit, &attributes, &config, pin);
}

NTSTATUS wmidi_circuit_create(WDFDEVICE device, ACXCIRCUIT* out) {
    PAGED_CODE();
    PACXCIRCUIT_INIT init = AcxCircuitInitAllocate(device);
    if (!init) return STATUS_INSUFFICIENT_RESOURCES;
    AcxCircuitInitSetComponentId(init, &midi_component);
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, L"OzzyMidi");
    (VOID)AcxCircuitInitAssignName(init, &name);
    AcxCircuitInitSetCircuitType(init, AcxCircuitTypeOther);
    NTSTATUS status = AcxCircuitInitAssignAcxCreateStreamCallback(init, wmidi_evt_create_stream);
    if (NT_SUCCESS(status))
        status = AcxCircuitInitAssignAcxRequestPreprocessCallback(init, wmidi_evt_dataranges, NULL,
                                                                 AcxRequestTypeProperty, &KSPROPSETID_Pin,
                                                                 KSPROPERTY_PIN_DATARANGES);
    if (!NT_SUCCESS(status)) {
        AcxCircuitInitFree(init);
        return status;
    }

    /* ACX refuses WDF_NO_OBJECT_ATTRIBUTES here (STATUS_INVALID_PARAMETER) */
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    ACXCIRCUIT circuit;
    status = AcxCircuitCreate(device, &attributes, &init, &circuit);
    if (!NT_SUCCESS(status)) {
        AcxCircuitInitFree(init);
        return status;
    }

    /* MIDI out: the stream pin into the device; MIDI in: from the device out to the stream pin */
    ACXPIN pins[4];
    status = wmidi_pin(circuit, AcxPinTypeSink, AcxPinCommunicationSink, false, &pins[WMIDI_PIN_OUT]);
    if (NT_SUCCESS(status))
        status = wmidi_pin(circuit, AcxPinTypeSource, AcxPinCommunicationNone, true, &pins[WMIDI_PIN_OUT_BRIDGE]);
    if (NT_SUCCESS(status))
        status = wmidi_pin(circuit, AcxPinTypeSource, AcxPinCommunicationSink, false, &pins[WMIDI_PIN_IN]);
    if (NT_SUCCESS(status))
        status = wmidi_pin(circuit, AcxPinTypeSink, AcxPinCommunicationNone, true, &pins[WMIDI_PIN_IN_BRIDGE]);
    if (!NT_SUCCESS(status)) return status;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = circuit;
    ACX_DATAFORMAT_CONFIG format_config;
    ACX_DATAFORMAT_CONFIG_INIT_KS(&format_config, &midi_format);
    ACXDATAFORMAT format;
    status = AcxDataFormatCreate(device, &attributes, &format_config, &format);
    if (!NT_SUCCESS(status)) return status;
    for (int i = WMIDI_PIN_OUT; i <= WMIDI_PIN_IN; i += WMIDI_PIN_IN - WMIDI_PIN_OUT) {
        ACXDATAFORMATLIST formats = AcxPinGetRawDataFormatList(pins[i]);
        if (!formats) return STATUS_INSUFFICIENT_RESOURCES;
        status = AcxDataFormatListAddDataFormat(formats, format);
        if (!NT_SUCCESS(status)) return status;
    }

    status = AcxCircuitAddPins(circuit, pins, 4);
    if (!NT_SUCCESS(status)) return status;
    /* Connections name the circuit (or element) at each end, and the pins by id */
    ACX_CONNECTION connections[2];
    ACX_CONNECTION_INIT(&connections[0], circuit, circuit);
    connections[0].FromPin.Id = WMIDI_PIN_OUT;
    connections[0].ToPin.Id = WMIDI_PIN_OUT_BRIDGE;
    ACX_CONNECTION_INIT(&connections[1], circuit, circuit);
    connections[1].FromPin.Id = WMIDI_PIN_IN_BRIDGE;
    connections[1].ToPin.Id = WMIDI_PIN_IN;
    status = AcxCircuitAddConnections(circuit, connections, 2);
    if (!NT_SUCCESS(status)) return status;
    *out = circuit;
    return STATUS_SUCCESS;
}
