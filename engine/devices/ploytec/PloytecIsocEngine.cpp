#include "PloytecIsocEngine.h"
#include "PloytecEngine.h"
#include "PloytecIO.h"
#include "../../../common/devices/ploytec/ploytec_handshake.h"
#include "../../shared/ozzy_log.h"
#include <string.h>

// Feedback format (ploytec_defs.h)
static const struct isoc_fb_config kFeedback = PLOYTEC_ISOC_FEEDBACK;

PloytecIsocEngine::PloytecIsocEngine(const struct ozzy_device* device) {
    // Channels and rates per device (common/ozzy_devices.def); run at the highest rate
    uint32_t rates[OZZY_MAX_RATES];
    mModel = device;
    mDefaultRate = rates[ozzy_device_rates(device, rates) - 1];
    mSampleRate = mDefaultRate;

    // Endpoints found by type, as both platforms describe them (ploytec_defs.h)
    mProfile.interfaceCount = PLOYTEC_NUM_INTERFACES;
    mProfile.interfaceIndices[0] = 0;
    mProfile.interfaceIndices[1] = 1;
    mProfile.altSetting = PLOYTEC_ALT_SETTING;
    mProfile.pcmOut   = PLOYTEC_ISOC_EP_PCM_OUT;
    mProfile.midiOut  = PLOYTEC_ISOC_EP_MIDI_OUT;
    mProfile.midiIn   = PLOYTEC_ISOC_EP_MIDI_IN;
    mProfile.pcmIn    = PLOYTEC_ISOC_EP_PCM_IN;
    mProfile.feedback = PLOYTEC_ISOC_EP_FEEDBACK;
}

void PloytecIsocEngine::SetEndpoint(OzzyPipeRole role, uint8_t address, uint8_t transferType,
                                    uint16_t maxPacketSize, uint8_t interval) {
    switch (role) {
    case kOzzyPipePcmOut:   mOutAddr = address; mOutMaxPacket = maxPacketSize; mOutInterval = interval; break;
    case kOzzyPipePcmIn:    mInAddr = address; break;
    case kOzzyPipeMidiIn:   mMidiInAddr = address; break;
    case kOzzyPipeMidiOut:  mMidiOutAddr = address; break;
    case kOzzyPipeFeedback: mFbAddr = address; mFbMaxPacket = maxPacketSize; mFbInterval = interval; break;
    default: break;
    }
}

bool PloytecIsocEngine::Start() {
    auto* shm = GetSHM();
    if (!shm) return false;
    if (!mOutAddr || !mInAddr) {
        ploytec_log("isoc: PCM %s endpoint not found", mOutAddr ? "in" : "out");
        return false;
    }

    // 1. What the HAL needs: a plain frame ring for output, Ploytec packets for input
    uint32_t frameBytes = mModel->out_channels * 3;
    shm->audio.device_flags = OZZY_DEVICE_FLAG_PLAIN_OUTPUT;
    shm->audio.frames_per_packet = PLOYTEC_FRAMES_PER_PKT;
    shm->audio.samples_per_packet = PLOYTEC_FRAMES_PER_OUT_SUBPKT;
    shm->audio.output_bytes_per_frame = frameBytes;
    shm->audio.input_bytes_per_frame = PLOYTEC_IN_FRAME_SIZE;
    shm->audio.input_channels = mModel->in_channels;
    shm->audio.output_channels = mModel->out_channels;
    ploytec_log("model: %s, %u in / %u out", mModel->name, mModel->in_channels, mModel->out_channels);

    // 2. Handshake with the settings, with the rate requests on the PCM endpoints found;
    // locked to S/PDIF it sets the lock rate
    struct ploytec_rate_endpoints eps = { mInAddr, mOutAddr };
    struct ploytec_handshake_info hs = {};
    if (ploytec_handshake_eps(&kPloytecHostOps, mBus, mDefaultRate, &eps, mModel->quirks,
                              &mSettings, &hs) < 0)
        return false;
    if (!ozzy_device_has_rate(mModel, hs.rate)) {
        // Locked to an S/PDIF rate the device doesn't list (Linux: ploytec_adopt_rate)
        ploytec_log("device runs at %u Hz, which it doesn't list", hs.rate);
        return false;
    }
    mSampleRate = hs.rate;
    shm->audio.sample_rate = mSampleRate;
    shm->audio.safety_offset_frames = OzzyIsocOutput::SafetyOffsetFrames(mSampleRate);
    PublishSettings(hs.clock);
    // MIDI framing on its own endpoints, as the device's kInACR picks it (ploytec_midi.h)
    ploytec_midi_init_xfer(&mMidi, mSampleRate, (mModel->quirks & OZZY_QUIRK_MIDI_NS7) ?
                           PLOYTEC_MIDI_XFER_NS7 : PLOYTEC_MIDI_XFER_M4_C1);

    ozzy_store_release_bool(&shm->audio.hardware_present, true);
    shm->audio.timestamp.sample_time = 0;
    ozzy_store_release_bool(&mMidiOutBusy, false);
    mRunning = true;

    // 3. Transfers: PCM in, PCM out with feedback, MIDI in; MIDI out from the timer
    for (uint32_t i = 0; i < kInTransfers; i++)
        if (!PloytecSubmitPCMIn(mBus, shm, mInAddr, i)) ploytec_log("isoc: input submission failed (idx: %u)", i);

    OzzyIsocOutput::Config out = {};
    out.rate = mSampleRate;
    out.frameBytes = frameBytes;
    out.ringFrames = OZZY_NUM_PACKETS * PLOYTEC_FRAMES_PER_PKT;   // the HAL's ring size
    out.anchorFrames = (uint64_t)OZZY_TIMESTAMP_PACKETS * PLOYTEC_FRAMES_PER_PKT;
    out.outAddr = mOutAddr;
    out.outMaxPacket = mOutMaxPacket;
    out.outInterval = mOutInterval;
    out.fbAddr = mFbAddr;
    out.fbMaxPacket = mFbMaxPacket;
    out.fbInterval = mFbInterval;
    out.feedback = kFeedback;
    if (!mOutput.Start(mBus, shm, out)) return false;

    for (uint32_t i = 0; mMidiInAddr && i < 2; i++) SubmitMIDIIn(i);
    if (mMidiOutAddr) mBus->StartTimer(PLOYTEC_MIDI_XFER_INTERVAL_US);

    ozzy_store_release_bool(&shm->audio.driver_ready, true);
    return true;
}

void PloytecIsocEngine::Suspend() {
    mRunning = false;
    mOutput.Stop();
    if (mBus) mBus->StopTimer();
    ploytec_log("isoc engine suspended");
}

void PloytecIsocEngine::Stop() {
    mRunning = false;
    mOutput.Stop();
    if (mBus) mBus->StopTimer();
    auto* shm = GetSHM();
    if (!shm) return;
    shm->audio.driver_ready = false;
    shm->audio.hardware_present = false;
    ploytec_log("isoc engine stopped");
}

void PloytecIsocEngine::OnIsocComplete(uint8_t pipeID, void* ctx, int status, OzzyIsocPacket* packets, uint32_t count) {
    if (mBus && mRunning) mOutput.OnIsocComplete(pipeID, ctx, status, packets, count);
}

void PloytecIsocEngine::OnPacketComplete(uint8_t pipeID, void* ctx, int status, uint32_t bytes) {
    if (!mBus || !mRunning || status == OZZY_STATUS_ABORTED) return;
    uint32_t idx = (uint32_t)(uintptr_t)ctx;
    auto* shm = GetSHM();

    if (pipeID == mInAddr) {
        if (!PloytecSubmitPCMIn(mBus, shm, mInAddr, idx + kInTransfers))
            ploytec_log("isoc: input submission failed (idx: %u)", idx + kInTransfers);
    } else if (pipeID == mMidiInAddr) {
        const uint8_t* buf = (const uint8_t*)shm->midi_in_usb_buffer + idx * kMidiInSlot;
        if (status == 0) PloytecMidiIn(&mMidi, shm, buf, bytes < kMidiInSlot ? bytes : kMidiInSlot);
        SubmitMIDIIn(idx);
    } else if (pipeID == mMidiOutAddr) {
        if (status != 0) ploytec_log("isoc: MIDI output error 0x%x", status);
        ozzy_store_release_bool(&mMidiOutBusy, false);
    }
}

/* MIDI IN reads of the framing's transfer size (AJ::configurationDone), one slot each */
void PloytecIsocEngine::SubmitMIDIIn(uint32_t slot) {
    auto* shm = GetSHM();
    void* buf = (void*)(shm->midi_in_usb_buffer + slot * kMidiInSlot);
    if (!mBus->SubmitUSBPacket(mMidiInAddr, buf, mMidi.xfer_size, (void*)(uintptr_t)slot))
        ploytec_log("isoc: MIDI input submission failed (slot %u)", slot);
}

/* Every millisecond (bus timer): the next MIDI OUT transfer if the previous one is done.
 * Like the official driver it also sends idle transfers. Same pacing as Linux
 * (ozzy_midi_out_tick). */
void PloytecIsocEngine::OnTimer() {
    if (mRunning) SendMidiOut();
}

void PloytecIsocEngine::SendMidiOut() {
    auto* shm = GetSHM();
    if (!shm || !mMidiOutAddr) return;
    if (ozzy_xchg_acquire_bool(&mMidiOutBusy, true)) return;

    uint8_t* buf = (uint8_t*)shm->midi_out_usb_buffer;
    uint32_t n = ploytec_midi_xfer_begin(&mMidi, buf);
    ozzy_midi_ring_pop(&shm->midi_out, buf, n);

    if (!mBus->SubmitUSBPacket(mMidiOutAddr, buf, mMidi.xfer_size, nullptr)) {
        ploytec_log("isoc: MIDI output submission failed");
        ozzy_store_release_bool(&mMidiOutBusy, false);
    }
}
