#ifndef OzzyIsocOutput_h
#define OzzyIsocOutput_h

#include "IOzzyBus.h"
#include "../shared/OzzySharedData.h"
#include "../../common/isoc_feedback.h"

/*
 * Isochronous PCM out for any device whose output takes plain interleaved frames, paced
 * by a feedback endpoint (common/isoc_feedback.h, the same pacing as Linux's core). The
 * macOS counterpart of the Linux core's isochronous output: an engine owns one of these
 * and forwards its isochronous completions.
 *
 * The HAL writes frames into a ring in shared memory (kOzzyDeviceFlagPlainOutput), and each
 * microframe's transaction points straight into it (zero copy; a packet that runs past the
 * end wraps). The output clock is the device's: the frames of completed transfers advance
 * the sample time, and the timestamp anchors use the completion time of the microframe that
 * reaches each one.
 */
class OzzyIsocOutput {
public:
    struct Config {
        uint32_t rate;
        uint32_t frameBytes;        // bytes per plain frame (channels * 3)
        uint32_t ringFrames;        // frames in the output ring
        uint64_t anchorFrames;      // timestamp anchor spacing (the HAL's zero timestamp period)
        uint8_t  outAddr;
        uint16_t outMaxPacket;      // bytes per service interval
        uint8_t  outInterval;       // bInterval
        uint8_t  fbAddr;            // 0: no feedback endpoint, nominal rate
        uint16_t fbMaxPacket;
        uint8_t  fbInterval;
        struct isoc_fb_config feedback;
    };

    // Frames the output reads ahead of the clock at @rate (the HAL's safety offset)
    static uint32_t SafetyOffsetFrames(uint32_t rate) {
        return ISOC_OUT_TRANSFERS * isoc_fb_max_frames(rate, ISOC_TRANSFER_UFRAMES);
    }

    // Clear the ring, reset the clock, submit the feedback and OUT transfers
    bool Start(IOzzyBus* bus, volatile OzzySharedMemory* shm, const Config& config);
    void Stop() { mRunning = false; }

    // An isochronous completion; false if it isn't one of this output's pipes
    bool OnIsocComplete(uint8_t pipeID, void* ctx, int status, OzzyIsocPacket* packets, uint32_t count);

private:
    struct OutTransfer {
        OzzyIsocPacket packets[kOzzyMaxIsocPackets];
        uint32_t frames[kOzzyMaxIsocPackets];
        uint32_t count;
    };
    struct FbTransfer {
        OzzyIsocPacket packets[kOzzyMaxIsocPackets];
        uint32_t count;
    };

    bool SubmitOut(uint32_t i);
    bool SubmitFeedback(uint32_t i);
    void PublishTimestamps(const OutTransfer& t);

    IOzzyBus* mBus = nullptr;
    volatile OzzySharedMemory* mSHM = nullptr;
    Config mConfig = {};
    volatile bool mRunning = false;

    struct isoc_fb mFb;
    uint32_t mOutUframes = 1;        // microframes per OUT packet
    uint32_t mOutMaxFrames = 0;      // frames that fit one OUT packet
    uint64_t mOutFrame = 0;          // next frame to send (ring position)
    uint64_t mPlayedFrames = 0;      // frames of completed transfers (the device's sample time)
    uint64_t mNextAnchor = 0;        // sample time of the next timestamp anchor
    bool mFbLogged = false;
    OutTransfer mOut[ISOC_OUT_TRANSFERS];
    FbTransfer mFbXfer[ISOC_FB_TRANSFERS];
};

#endif
