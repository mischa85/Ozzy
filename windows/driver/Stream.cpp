#include "Stream.h"

OzzyStream::~OzzyStream() {
    Detach();
}

uint32_t OzzyStream::InFlightFrames() const {
    // PloytecEngine keeps two PCM out packets in flight ahead of the one it encodes
    return mRender && mDev->shm ? 2 * mDev->shm->audio.frames_per_packet : 0;
}

/* The ring the engine uses is the packets' data, so it has to be contiguous. Packet sizes from
 * the audio engine are rarely whole pages, so for two packets (the usual case) use one
 * allocation laid out so that packet 0 ends on a page boundary where packet 1 starts: each
 * packet's MDL then covers its own pages, and the data runs on from one to the other. Any
 * other count needs page-sized packets. The pages are zeroed (no kernel data reaches the
 * mapping). */
NTSTATUS OzzyStream::AllocateRtPackets(ULONG count, ULONG size, PACX_RTPACKET* out) {
    PAGED_CODE();
    if (!count || count > kMaxPackets || !size || size % mBlockAlign) return STATUS_INVALID_PARAMETER;
    ULONG pageSize = ROUND_TO_PAGES(size);
    bool pages = size == pageSize;
    if (count != 2 && !pages) return STATUS_NOT_SUPPORTED;
    // With two packets, packet 0's data starts pageSize - size into its page span
    mDataOffset = pages ? 0 : pageSize - size;
    ULONG total = pages ? count * size : 2 * pageSize;

    PACX_RTPACKET packets = (PACX_RTPACKET)ExAllocatePool2(POOL_FLAG_NON_PAGED, count * sizeof(ACX_RTPACKET),
                                                           OZZY_POOL_TAG);
    mBuffer = (uint8_t*)ExAllocatePool2(POOL_FLAG_NON_PAGED, total, OZZY_POOL_TAG);
    if (!packets || !mBuffer) {
        if (packets) ExFreePoolWithTag(packets, OZZY_POOL_TAG);
        FreeRtPackets(nullptr, 0);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (ULONG i = 0; i < count; i++) {
        ACX_RTPACKET_INIT(&packets[i]);
        uint8_t* span = pages ? mBuffer + i * size : mBuffer + i * pageSize;
        ULONG spanSize = pages ? size : pageSize;
        mMdl[i] = IoAllocateMdl(span, spanSize, FALSE, FALSE, nullptr);
        if (!mMdl[i]) {
            ExFreePoolWithTag(packets, OZZY_POOL_TAG);
            FreeRtPackets(nullptr, 0);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        MmBuildMdlForNonPagedPool(mMdl[i]);
        WDF_MEMORY_DESCRIPTOR_INIT_MDL(&packets[i].RtPacketBuffer, mMdl[i], spanSize);
        packets[i].RtPacketSize = size;
        packets[i].RtPacketOffset = i == 0 ? mDataOffset : 0;
    }
    mPacketCount = count;
    mPacketFrames = size / mBlockAlign;
    *out = packets;
    return STATUS_SUCCESS;
}

/* Also cleans up after a failed AllocateRtPackets (@packets nullptr) */
void OzzyStream::FreeRtPackets(PACX_RTPACKET packets, ULONG count) {
    PAGED_CODE();
    UNREFERENCED_PARAMETER(count);
    Detach();
    for (ULONG i = 0; i < kMaxPackets; i++) {
        if (mMdl[i]) IoFreeMdl(mMdl[i]);
        mMdl[i] = nullptr;
    }
    if (mBuffer) ExFreePoolWithTag(mBuffer, OZZY_POOL_TAG);
    mBuffer = nullptr;
    mPacketCount = 0;
    if (packets) ExFreePoolWithTag(packets, OZZY_POOL_TAG);
}

/* Hand the ring to the engine, continuing from where the stream stopped: the engine picks it
 * up with its next packet and reports every packet from then on */
NTSTATUS OzzyStream::Run() {
    PAGED_CODE();
    if (!mDev->engine || !mBuffer) return STATUS_DEVICE_NOT_READY;

    uint64_t pos = Position();
    mRing.base = mBuffer + mDataOffset;
    mRing.frames = mPacketCount * mPacketFrames;
    mRing.channels = mBlockAlign / 3;
    mRing.start = pos;
    mBoundaryQpc = (uint64_t)KeQueryPerformanceCounter(nullptr).QuadPart;
    InterlockedExchange64(&mCompleted, (LONG64)(pos / mPacketFrames));

    mDev->bus->SetPcmClient(Dir(), this);
    ozzy_engine_set_pcm_ring(mDev->engine, Dir(), &mRing);
    mAttached = true;
    return STATUS_SUCCESS;
}

NTSTATUS OzzyStream::Pause() {
    PAGED_CODE();
    Detach();
    return STATUS_SUCCESS;
}

/* Take the ring back from the engine and wait out completions that may still use it */
void OzzyStream::Detach() {
    if (!mAttached) return;
    if (mDev->engine) ozzy_engine_set_pcm_ring(mDev->engine, Dir(), nullptr);
    if (mDev->bus) {
        mDev->bus->SetPcmClient(Dir(), nullptr);
        mDev->bus->WaitCompletions();
    }
    mAttached = false;
}

/* The engine moved a packet through the ring: complete every WaveRT packet it has finished
 * (render: read all of it; capture: written all of it) */
void OzzyStream::OnPcmProgress(uint64_t frames, uint64_t qpc) {
    InterlockedExchange64(&mPos, (LONG64)frames);
    InterlockedExchange64(&mPosQpc, (LONG64)qpc);
    uint64_t done = frames / mPacketFrames;
    uint64_t completed = (uint64_t)mCompleted;
    while (completed < done) {
        InterlockedExchange64(&mLastStartQpc, (LONG64)mBoundaryQpc);
        InterlockedExchange64(&mCompleted, (LONG64)(completed + 1));
        AcxRtStreamNotifyPacketComplete(mStream, completed, qpc);
        mBoundaryQpc = qpc;
        completed++;
    }
}

void OzzyStream::GetHwLatency(ULONG* fifoSize, ULONG* delay) {
    *fifoSize = InFlightFrames() * mBlockAlign;
    *delay = 0;
}

/* The audio engine wrote @packet. The engine reads it as the device's clock reaches it, so
 * there is nothing to start; a packet behind the one being read came too late. */
NTSTATUS OzzyStream::SetRenderPacket(ULONG packet, ULONG flags, ULONG eosLength) {
    UNREFERENCED_PARAMETER(flags);
    UNREFERENCED_PARAMETER(eosLength);
    ULONG current = (ULONG)(Position() / mPacketFrames);
    if (packet < current) return STATUS_DATA_LATE_ERROR;
    return STATUS_SUCCESS;
}

NTSTATUS OzzyStream::GetCurrentPacket(PULONG packet) {
    *packet = (ULONG)(Position() / mPacketFrames);
    return STATUS_SUCCESS;
}

NTSTATUS OzzyStream::GetCapturePacket(ULONG* lastPacket, ULONGLONG* qpcPacketStart, BOOLEAN* moreData) {
    // Before the first packet completes this is packet -1, as the ACX sample reports it
    LONG64 completed = InterlockedCompareExchange64(&mCompleted, 0, 0);
    *lastPacket = (ULONG)(completed - 1);
    *qpcPacketStart = (ULONGLONG)InterlockedCompareExchange64(&mLastStartQpc, 0, 0);
    *moreData = FALSE;
    return STATUS_SUCCESS;
}

/* Frames at the device: render is behind the ring position by the packets in flight */
NTSTATUS OzzyStream::GetPresentationPosition(PULONGLONG position, PULONGLONG qpc) {
    uint64_t pos = Position();
    uint32_t behind = InFlightFrames();
    *position = pos > behind ? pos - behind : 0;
    *qpc = (ULONGLONG)InterlockedCompareExchange64(&mPosQpc, 0, 0);
    return STATUS_SUCCESS;
}

// --- ACX callbacks: forward to the stream object ---

static OzzyStream* StreamOf(ACXSTREAM stream) {
    return GetOzzyStream(stream)->stream;
}

static NTSTATUS EvtPrepareHardware(ACXSTREAM) { return STATUS_SUCCESS; }
static NTSTATUS EvtReleaseHardware(ACXSTREAM) { return STATUS_SUCCESS; }
static NTSTATUS EvtRun(ACXSTREAM stream) { return StreamOf(stream)->Run(); }
static NTSTATUS EvtPause(ACXSTREAM stream) { return StreamOf(stream)->Pause(); }

static NTSTATUS EvtGetHwLatency(ACXSTREAM stream, ULONG* fifoSize, ULONG* delay) {
    StreamOf(stream)->GetHwLatency(fifoSize, delay);
    return STATUS_SUCCESS;
}
static NTSTATUS EvtAllocateRtPackets(ACXSTREAM stream, ULONG count, ULONG size, PACX_RTPACKET* packets) {
    return StreamOf(stream)->AllocateRtPackets(count, size, packets);
}
static VOID EvtFreeRtPackets(ACXSTREAM stream, PACX_RTPACKET packets, ULONG count) {
    StreamOf(stream)->FreeRtPackets(packets, count);
}
static NTSTATUS EvtSetRenderPacket(ACXSTREAM stream, ULONG packet, ULONG flags, ULONG eosLength) {
    return StreamOf(stream)->SetRenderPacket(packet, flags, eosLength);
}
static NTSTATUS EvtGetCapturePacket(ACXSTREAM stream, ULONG* last, ULONGLONG* qpcStart, BOOLEAN* more) {
    return StreamOf(stream)->GetCapturePacket(last, qpcStart, more);
}
static NTSTATUS EvtGetCurrentPacket(ACXSTREAM stream, PULONG packet) {
    return StreamOf(stream)->GetCurrentPacket(packet);
}
static NTSTATUS EvtGetPresentationPosition(ACXSTREAM stream, PULONGLONG position, PULONGLONG qpc) {
    return StreamOf(stream)->GetPresentationPosition(position, qpc);
}

static VOID EvtDestroy(WDFOBJECT object) {
    OzzyStreamContext* ctx = GetOzzyStream((ACXSTREAM)object);
    delete ctx->stream;
    ctx->stream = nullptr;
}

NTSTATUS OzzyEvtCreateStream(WDFDEVICE device, ACXCIRCUIT circuit, ACXPIN pin, PACXSTREAM_INIT init,
                             ACXDATAFORMAT format, const GUID* signalProcessingMode, ACXOBJECTBAG varArguments) {
    UNREFERENCED_PARAMETER(pin);
    UNREFERENCED_PARAMETER(signalProcessingMode);
    UNREFERENCED_PARAMETER(varArguments);
    PAGED_CODE();
    bool render = GetOzzyCircuit(circuit)->render;

    ACX_STREAM_CALLBACKS callbacks;
    ACX_STREAM_CALLBACKS_INIT(&callbacks);
    callbacks.EvtAcxStreamPrepareHardware = EvtPrepareHardware;
    callbacks.EvtAcxStreamReleaseHardware = EvtReleaseHardware;
    callbacks.EvtAcxStreamRun = EvtRun;
    callbacks.EvtAcxStreamPause = EvtPause;
    NTSTATUS status = AcxStreamInitAssignAcxStreamCallbacks(init, &callbacks);
    if (!NT_SUCCESS(status)) return status;

    ACX_RT_STREAM_CALLBACKS rt;
    ACX_RT_STREAM_CALLBACKS_INIT(&rt);
    rt.EvtAcxStreamGetHwLatency = EvtGetHwLatency;
    rt.EvtAcxStreamAllocateRtPackets = EvtAllocateRtPackets;
    rt.EvtAcxStreamFreeRtPackets = EvtFreeRtPackets;
    if (render) rt.EvtAcxStreamSetRenderPacket = EvtSetRenderPacket;
    else rt.EvtAcxStreamGetCapturePacket = EvtGetCapturePacket;
    rt.EvtAcxStreamGetCurrentPacket = EvtGetCurrentPacket;
    rt.EvtAcxStreamGetPresentationPosition = EvtGetPresentationPosition;
    status = AcxStreamInitAssignAcxRtStreamCallbacks(init, &rt);
    if (!NT_SUCCESS(status)) return status;
    AcxStreamInitSetAcxRtStreamSupportsNotifications(init);

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, OzzyStreamContext);
    attributes.EvtDestroyCallback = EvtDestroy;
    ACXSTREAM stream;
    status = AcxRtStreamCreate(device, circuit, &attributes, &init, &stream);
    if (!NT_SUCCESS(status)) return status;

    OzzyStream* s = new OzzyStream(stream, GetOzzyDevice(device), render, AcxDataFormatGetBlockAlign(format));
    if (!s) return STATUS_INSUFFICIENT_RESOURCES;
    GetOzzyStream(stream)->stream = s;
    return STATUS_SUCCESS;
}
