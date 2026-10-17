#ifndef OzzyStream_h
#define OzzyStream_h

#include "Device.h"

// One ACX RT stream (event-driven). Its WaveRT packets are one contiguous S24_3LE ring that
// the engine reads (render) or writes (capture) in the USB completion path; OnPcmProgress,
// called from there, turns the ring position into packet completions and positions.
class OzzyStream final : public IOzzyPcmClient {
public:
    OzzyStream(ACXSTREAM stream, OzzyDeviceContext* dev, bool render, uint32_t blockAlign)
        : mStream(stream), mDev(dev), mRender(render), mBlockAlign(blockAlign) {}
    ~OzzyStream();

    NTSTATUS AllocateRtPackets(ULONG count, ULONG size, PACX_RTPACKET* packets);
    void FreeRtPackets(PACX_RTPACKET packets, ULONG count);
    NTSTATUS Run();
    NTSTATUS Pause();
    void GetHwLatency(ULONG* fifoSize, ULONG* delay);
    NTSTATUS SetRenderPacket(ULONG packet, ULONG flags, ULONG eosLength);
    NTSTATUS GetCurrentPacket(PULONG packet);
    NTSTATUS GetCapturePacket(ULONG* lastPacket, ULONGLONG* qpcPacketStart, BOOLEAN* moreData);
    NTSTATUS GetPresentationPosition(PULONGLONG position, PULONGLONG qpc);

    void OnPcmProgress(uint64_t frames, uint64_t qpc) override;   // DISPATCH_LEVEL

private:
    static constexpr ULONG kMaxPackets = 8;

    void Detach();
    uint64_t Position() { return (uint64_t)InterlockedCompareExchange64(&mPos, 0, 0); }
    enum ozzy_pcm_dir Dir() const { return mRender ? OZZY_PCM_OUT : OZZY_PCM_IN; }
    // Frames between the ring position and the device: the PCM out packets in flight
    uint32_t InFlightFrames() const;

    ACXSTREAM mStream;
    OzzyDeviceContext* mDev;
    bool mRender;
    uint32_t mBlockAlign;

    // The packets: one allocation, the data contiguous from mBuffer + mDataOffset
    uint8_t* mBuffer = nullptr;
    ULONG mDataOffset = 0;
    ULONG mPacketCount = 0;
    ULONG mPacketFrames = 0;
    PMDL mMdl[kMaxPackets] = {};

    struct ozzy_pcm_ring mRing = {};
    bool mAttached = false;

    // Written in OnPcmProgress, read by the RT callbacks
    volatile LONG64 mPos = 0;              // ring position: frames since the first Run
    volatile LONG64 mPosQpc = 0;
    volatile LONG64 mCompleted = 0;        // packets completed (notified)
    volatile LONG64 mLastStartQpc = 0;     // capture: when the last completed packet started
    uint64_t mBoundaryQpc = 0;             // when the packet in progress started (DISPATCH only)
};

struct OzzyStreamContext {
    OzzyStream* stream;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(OzzyStreamContext, GetOzzyStream)

// The circuits' EvtAcxCircuitCreateStream
NTSTATUS OzzyEvtCreateStream(WDFDEVICE device, ACXCIRCUIT circuit, ACXPIN pin, PACXSTREAM_INIT init,
                             ACXDATAFORMAT format, const GUID* signalProcessingMode, ACXOBJECTBAG varArguments);

#endif
