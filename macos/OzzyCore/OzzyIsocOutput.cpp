#include "OzzyIsocOutput.h"
#include "../Shared/OzzyLog.h"
#include <IOKit/IOReturn.h>
#include <string.h>

static const struct isoc_fb_config kNoFeedback = { ISOC_FB_NONE, 0, 0 };

bool OzzyIsocOutput::Start(IOzzyBus* bus, volatile OzzySharedMemory* shm, const Config& config) {
    mBus = bus;
    mSHM = shm;
    mConfig = config;

    uint32_t ringBytes = config.ringFrames * config.frameBytes;
    if (!bus || !shm || !config.outAddr || ringBytes > sizeof(shm->audio.outputBuffer)) return false;

    bool fbFits = config.fbAddr &&
                  ISOC_FB_TRANSFERS * ISOC_TRANSFER_UFRAMES * config.fbMaxPacket <= sizeof(shm->isocInUSBBuffer);
    if (config.fbAddr && !fbFits) LogOzzyKext("isoc: feedback wMaxPacketSize %u too large", config.fbMaxPacket);
    if (!isoc_fb_init(&mFb, config.rate, fbFits ? &config.feedback : &kNoFeedback, fbFits ? config.fbInterval : 0))
        LogOzzyKext("isoc: feedback bInterval %u not supported, PCM out runs at the nominal rate", config.fbInterval);

    mOutUframes = isoc_fb_interval_uframes(config.outInterval);
    mOutMaxFrames = config.outMaxPacket / config.frameBytes;
    if (mOutMaxFrames < isoc_fb_max_frames(config.rate, mOutUframes))
        LogOzzyKext("isoc: PCM out wMaxPacketSize %u is too small for %u Hz", config.outMaxPacket, config.rate);
    LogOzzyKext("isoc out: %u microframe(s) per packet, up to %u frames, feedback every %u microframe(s)",
                mOutUframes, mOutMaxFrames, mFb.uframes);

    // Silence in the ring, clock at 0
    memset((void*)shm->audio.outputBuffer, 0, ringBytes);
    mOutFrame = 0;
    mPlayedFrames = 0;
    mNextAnchor = config.anchorFrames;
    mFbLogged = false;
    mRunning = true;

    for (uint32_t i = 0; fbFits && i < ISOC_FB_TRANSFERS; i++) SubmitFeedback(i);
    for (uint32_t i = 0; i < ISOC_OUT_TRANSFERS; i++) SubmitOut(i);
    return true;
}

/* OUT transfer @i: the frames the device consumed in each packet's interval
 * (isoc_fb_layout, shared with Linux), pointed at in the frame ring */
bool OzzyIsocOutput::SubmitOut(uint32_t i) {
    OutTransfer& t = mOut[i];

    t.count = isoc_fb_packets(mOutUframes);
    isoc_fb_layout(&mFb, t.count, mOutUframes, mOutMaxFrames, t.frames);
    for (uint32_t p = 0; p < t.count; p++) {
        t.packets[p].offset = (uint32_t)(mOutFrame % mConfig.ringFrames) * mConfig.frameBytes;
        t.packets[p].length = t.frames[p] * mConfig.frameBytes;
        mOutFrame += t.frames[p];
    }
    if (mBus->SubmitIsoc(mConfig.outAddr, (void*)mSHM->audio.outputBuffer, mConfig.ringFrames * mConfig.frameBytes,
                         t.packets, t.count, (void*)(uintptr_t)i))
        return true;
    LogOzzyKext("isoc: output submission failed (transfer %u)", i);
    return false;
}

bool OzzyIsocOutput::SubmitFeedback(uint32_t i) {
    FbTransfer& t = mFbXfer[i];
    uint32_t base = i * ISOC_TRANSFER_UFRAMES * mConfig.fbMaxPacket;

    t.count = isoc_fb_packets(mFb.uframes);
    for (uint32_t p = 0; p < t.count; p++) {
        t.packets[p].offset = base + p * mConfig.fbMaxPacket;
        t.packets[p].length = mConfig.fbMaxPacket;
    }
    if (mBus->SubmitIsoc(mConfig.fbAddr, (void*)mSHM->isocInUSBBuffer, sizeof(mSHM->isocInUSBBuffer),
                         t.packets, t.count, (void*)(uintptr_t)i))
        return true;
    LogOzzyKext("isoc: feedback submission failed (transfer %u)", i);
    return false;
}

/* Advance the device's sample time by the frames this transfer played, and publish an anchor
 * (seqlock, reader: OzzyHAL::GetZeroTimeStamp) in the microframe that reaches each multiple of
 * anchorFrames, at that microframe's completion time */
void OzzyIsocOutput::PublishTimestamps(const OutTransfer& t) {
    for (uint32_t p = 0; p < t.count; p++) {
        mPlayedFrames += t.frames[p];
        if (mPlayedFrames < mNextAnchor) continue;

        volatile auto* ts = &mSHM->audio.timestamp;
        uint32_t seq = ts->sequence;
        OzzyStoreRelaxed(&ts->sequence, seq + 1);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        OzzyStoreRelaxed(&ts->sampleTime, mNextAnchor);
        OzzyStoreRelaxed(&ts->hostTime, t.packets[p].time ? t.packets[p].time : mBus->GetTime());
        OzzyStoreRelease(&ts->sequence, seq + 2);
        mNextAnchor += mConfig.anchorFrames;
    }
}

bool OzzyIsocOutput::OnIsocComplete(uint8_t pipeID, void* ctx, int status, OzzyIsocPacket* packets, uint32_t count) {
    uint32_t i = (uint32_t)(uintptr_t)ctx;

    if (pipeID == mConfig.outAddr) {
        if (!mRunning || status == (int)kIOReturnAborted) return true;
        PublishTimestamps(mOut[i]);
        SubmitOut(i);
        return true;
    }
    if (pipeID == mConfig.fbAddr && mConfig.fbAddr) {
        if (!mRunning || status == (int)kIOReturnAborted) return true;
        for (uint32_t p = 0; p < count; p++) {
            const uint8_t* pkt = (const uint8_t*)mSHM->isocInUSBBuffer + packets[p].offset;
            uint32_t len = packets[p].status == 0 ? packets[p].actual : 0;
            if (!mFbLogged) {
                mFbLogged = true;
                LogOzzyKext("isoc: first feedback packet: %u bytes %02x %02x %02x", len, pkt[0], pkt[1], pkt[2]);
            }
            if (isoc_fb_packet(&mFb, pkt, len))
                LogOzzyKext("isoc: more than %u feedback packets were not %u bytes long, PCM out runs at the nominal rate",
                            mFb.cfg.max_errors, mFb.cfg.packet_size);
        }
        SubmitFeedback(i);
        return true;
    }
    return false;
}
