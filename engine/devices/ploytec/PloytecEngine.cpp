#include "PloytecEngine.h"
#include "PloytecProtocol.h"
#include "PloytecIO.h"
#include "../../../common/devices/ploytec/ploytec_handshake.h"
#include "../../shared/OzzyLog.h"
#include <string.h>

// --- CONSTANTS ---
// Endpoints (direction bit OR'd with base address from ploytec_defs.h)
#define kEpPcmOut   PLOYTEC_EP_PCM_OUT
#define kEpPcmIn    (PLOYTEC_EP_PCM_IN | 0x80)
#define kEpMidiIn   (PLOYTEC_EP_MIDI_IN | 0x80)

// --- Host ops for the shared handshake (common/devices/ploytec/ploytec_handshake.c) ---
static int HostControl(void* ctx, uint8_t type, uint8_t req, uint16_t val, uint16_t idx, void* data, uint16_t len) {
    return ((IOzzyBus*)ctx)->VendorRequest(type, req, val, idx, data, len) ? len : -EIO;
}

static void HostSleep(void* ctx, unsigned int ms) {
    ((IOzzyBus*)ctx)->Sleep(ms);
}

static void HostLog(void* ctx, bool error, const char* fmt, va_list args) {
    char line[256];
    OzzyVsnprintf(line, sizeof(line), fmt, args);
    LogPloytec("%s", line);
}

// No clear_halt: the official macOS driver never clears the PCM endpoint halt
// (only the Windows driver does, see ploytec_handshake.c). Shared with PloytecIsocEngine.
const struct ploytec_host_ops kPloytecHostOps = { HostControl, nullptr, HostSleep, HostLog };

PloytecEngine::PloytecEngine(const struct ozzy_device* device) {

    // Channel counts and rates per device (common/ozzy_devices.def, shared with Linux); run at
    // the highest rate the device supports (the DB2 is 48 kHz only)
    uint32_t rates[OZZY_MAX_RATES];
    mModel = device;
    mDefaultRate = rates[ozzy_device_rates(device, rates) - 1];
    mSampleRate = mDefaultRate;

    // --- Define Hardware Topology ---
    // The Xone:DB4 presents two interfaces:
    // Intf 0: Control + PCM Out + MIDI In
    // Intf 1: PCM In
    
    mProfile.interfaceCount = PLOYTEC_NUM_INTERFACES;
    mProfile.interfaceIndices[0] = 0;
    mProfile.interfaceIndices[1] = 1;
    mProfile.altSetting = 1;
    
    // PCM Out (EP 0x05) lives on Interface 0
    mProfile.pcmOut.address = kEpPcmOut;
    mProfile.pcmOut.intf = 0;
    
    // PCM In (EP 0x86) lives on Interface 1
    mProfile.pcmIn.address = kEpPcmIn;
    mProfile.pcmIn.intf = 1;
    
    // MIDI In (EP 0x83) lives on Interface 0
    mProfile.midiIn.address = kEpMidiIn;
    mProfile.midiIn.intf = 0;
}

/* The output packet layout follows the endpoint's wMaxPacketSize, not its transfer type,
 * like the official driver (AudioBulkIRP::useOut): <= 512 uses the bulk layout, larger (964)
 * uses the interrupt layout. DB2 fw 1.2.0 is an interrupt endpoint with wMaxPacketSize 512. */
void PloytecEngine::SetEndpoint(OzzyPipeRole role, uint8_t address, uint8_t transferType,
                                uint16_t maxPacketSize, uint8_t interval) {
    if (role == kOzzyPipePcmOut) {
        mOutLayout = ploytec_out_layout_for_maxpacket(maxPacketSize);
        mPacketSizeOut = ploytec_out_packet_size(mOutLayout);
        LogPloytec("transport: %s, wMaxPacketSize %u (%s layout, out packet: %u bytes)",
                   transferType == 2 ? "bulk" : "interrupt", maxPacketSize,
                   mOutLayout == PLOYTEC_OUT_LAYOUT_BULK ? "bulk" : "interrupt", mPacketSizeOut);
    } else if (role == kOzzyPipePcmIn) {
        // Selects the MIDI IN frame above 48 kHz (ploytec_midi.h)
        mPcmInInt1024 = transferType == 3 && maxPacketSize == 1024;
    }
}

bool PloytecEngine::Start() {
    auto* shm = GetSHM();
    if(!shm) return false;
    
    // 1. Setup Shared Memory (device names already set by kext from USB descriptors)
    bool bulk = mOutLayout == PLOYTEC_OUT_LAYOUT_BULK;
    shm->audio.deviceFlags = (bulk ? kOzzyDeviceFlagPloytecBulk : 0) |
                             (bulk && (mModel->quirks & OZZY_QUIRK_SPDIF_MIRROR) ? kOzzyDeviceFlagSpdifMirror : 0);
    shm->audio.framesPerPacket = PLOYTEC_FRAMES_PER_PKT;  // 80 frames per logical packet
    shm->audio.samplesPerPacket = 10;  // 10 samples per USB sub-packet
    shm->audio.outputBytesPerFrame = PLOYTEC_OUT_FRAME_SIZE;
    shm->audio.inputBytesPerFrame = PLOYTEC_IN_FRAME_SIZE;
    shm->audio.inputChannels = mModel->in_channels;
    shm->audio.outputChannels = mModel->out_channels;
    LogPloytec("model: %s, %u in / %u out", mModel->name, mModel->in_channels, mModel->out_channels);
    
    // 2. Hardware Handshake Sequence, with the settings; locked to S/PDIF it sets the lock rate
    struct ploytec_handshake_info hs = {};
    if (ploytec_handshake(&kPloytecHostOps, mBus, mDefaultRate, mModel->quirks, &mSettings, &hs) < 0)
        return false;
    if (!ozzy_device_has_rate(mModel, hs.rate)) {
        // Locked to an S/PDIF rate the device doesn't list (Linux: ploytec_adopt_rate)
        LogPloytec("device runs at %u Hz, which it doesn't list", hs.rate);
        return false;
    }
    mSampleRate = hs.rate;
    shm->audio.sampleRate = mSampleRate;
    PublishSettings(hs.clock);
    ploytec_midi_init(&mMidi, mSampleRate, mPcmInInt1024);
    
    // Hardware is confirmed working and communicating (release: the format fields above first)
    OzzyStoreRelease(&shm->audio.hardwarePresent, true);

    // 3. Reset Engine State
    mHwSampleTime = 0;
    shm->audio.timestamp.sampleTime = 0;

    // 4. Fill every logical packet (kOzzyMaxPacketSize stride) with silence and idle MIDI bytes
    for (uint32_t logicalPacket = 0; logicalPacket < kOzzyNumPackets; logicalPacket++) {
        ploytec_out_init_packet(mOutLayout, (uint8_t*)shm->audio.outputBuffer + logicalPacket * kOzzyMaxPacketSize);
    }

    // 5. Kickstart USB transfers
    mRunning = true;
    for(uint32_t i=0; i<2; i++) {
        SubmitPCMIn(i);
        SubmitPCMOut(i);
        SubmitMIDIIn(i);
    }
    
    // Driver is now fully initialized and ready for I/O
    OzzyStoreRelease(&shm->audio.driverReady, true);

    return true;
}

void PloytecEngine::Suspend() {
    mRunning = false;
    LogPloytec("engine suspended");
}

void PloytecEngine::Stop() {
    mRunning = false;
    auto* shm = GetSHM();
    if (!shm) return;
    
    // Clear flags immediately to signal HAL
    shm->audio.driverReady = false;
    shm->audio.hardwarePresent = false;
    
    LogPloytec("engine stopped");
}

void PloytecEngine::ProcessMIDIOutput(uint32_t packetIdx) {
    auto* shm = GetSHM();
    if (!shm) return;
    
    // Take as many bytes from the midiOut ring as the MIDI out pace allows in this packet;
    // slots without data stay idle
    uint32_t mask;
    uint32_t count = ploytec_midi_out_slots(&mMidi, &mask);
    uint8_t bytes[PLOYTEC_OUT_SUBPKTS];
    memset(bytes, PLOYTEC_MIDI_IDLE_BYTE, sizeof(bytes));
    OzzyMidiRingPop(&shm->midiOut, bytes, count);

    uint8_t* packet = (uint8_t*)shm->audio.outputBuffer + (packetIdx % kOzzyNumPackets) * kOzzyMaxPacketSize;
    ploytec_midi_out_place(mOutLayout, packet, mask, bytes);
}

void PloytecEngine::ProcessMIDIInput(uint32_t bytesReceived) {
    auto* shm = GetSHM();
    if (!shm || bytesReceived == 0) return;
    
    // Only the MIDI bytes of the frame (ploytec_midi.h), into the midiIn ring
    PloytecMidiIn(&mMidi, shm, (const uint8_t*)shm->midiInUSBBuffer,
                  bytesReceived < sizeof(shm->midiInUSBBuffer) ? bytesReceived : sizeof(shm->midiInUSBBuffer));
}

void PloytecEngine::OnPacketComplete(uint8_t pipeID, void* ctx, int status, uint32_t bytes) {
    // Aborted (Suspend/Stop): no resubmit, a synchronous abort would accept it
    if (!mBus || !mRunning || status == OZZY_STATUS_ABORTED) return;

    uint32_t finishedIdx = (uint32_t)(uintptr_t)ctx;
    uint32_t nextIdx = finishedIdx + 2;

    // DIAGNOSTIC: Confirm we entered the function
    // mBus->Log("🧩 Engine: Pipe 0x%02X Pkt %d Done -> Queueing %d", pipeID, finishedIdx, nextIdx);

    if (pipeID == kEpPcmOut) { 
        UpdateTimestamp();
        
        // Process MIDI output: Write pending MIDI bytes to the packet's MIDI positions
        ProcessMIDIOutput(nextIdx);
        
        // Use modulo to wrap around the full ring buffer (128 packets)
        uint32_t packetIdx = nextIdx % kOzzyNumPackets;
        if (!mBus->SubmitUSBPacket(kEpPcmOut, 
                                 (uint8_t*)GetSHM()->audio.outputBuffer + (packetIdx * kOzzyMaxPacketSize), 
                                 mPacketSizeOut,
                                 (void*)(uintptr_t)nextIdx)) {
             LogPloytec("output submission failed (idx: %d)", nextIdx);
        }
    }
    else if (pipeID == kEpPcmIn) {
        if (!PloytecSubmitPCMIn(mBus, GetSHM(), kEpPcmIn, nextIdx))
            LogPloytec("input submission failed (idx: %u)", nextIdx);
    }
    else if (pipeID == kEpMidiIn) {
        // Process MIDI input packet
        ProcessMIDIInput(bytes);
        
        // Re-submit MIDI input buffer
        SubmitMIDIIn(nextIdx);
    }
}

void PloytecEngine::UpdateTimestamp() {
    auto* shm = GetSHM();
    if(!shm || !shm->audio.driverReady) return;

    mHwSampleTime += PLOYTEC_FRAMES_PER_PKT;

    if ((mHwSampleTime % (PLOYTEC_FRAMES_PER_PKT * kOzzyTimestampPackets)) == 0) {
        // Seqlock (reader: OzzyHAL::GetZeroTimeStamp). The fences keep the odd sequence
        // visible before the new values, and the new values before the even sequence.
        volatile auto* ts = &shm->audio.timestamp;
        uint32_t seq = ts->sequence;
        OzzyStoreRelaxed(&ts->sequence, seq + 1);
        OzzyFenceRelease();
        OzzyStoreRelaxed(&ts->sampleTime, mHwSampleTime);
        OzzyStoreRelaxed(&ts->hostTime, mBus->GetTime());
        OzzyStoreRelease(&ts->sequence, seq + 2);
    }
}

void PloytecEngine::SubmitPCMOut(uint32_t packetIdx) {
    auto* shm = GetSHM();
    // Use modulo to wrap around the full ring buffer (128 packets)
    uint32_t pIdx = packetIdx % kOzzyNumPackets;
    uint8_t* buffer = (uint8_t*)shm->audio.outputBuffer + (pIdx * kOzzyMaxPacketSize);
    
    mBus->SubmitUSBPacket(kEpPcmOut, buffer, mPacketSizeOut, (void*)(uintptr_t)packetIdx);
}

void PloytecEngine::SubmitPCMIn(uint32_t packetIdx) {
    PloytecSubmitPCMIn(mBus, GetSHM(), kEpPcmIn, packetIdx);
}

void PloytecEngine::SubmitMIDIIn(uint32_t idx) {
    auto* shm = GetSHM();
    if (!mBus->SubmitUSBPacket(kEpMidiIn, (void*)shm->midiInUSBBuffer, sizeof(shm->midiInUSBBuffer), (void*)(uintptr_t)idx)) {
        LogPloytec("MIDI input submission failed (idx: %u)", idx);
    }
}
