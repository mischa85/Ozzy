#include "PloytecEngine.h"
#include "PloytecProtocol.h"
#include "../../Shared/OzzyLog.h"

// --- CONSTANTS ---
#define kPloytecFramesPerPacket      80
#define kPloytecSyncInterval         640
#define kPloytecInterruptSize        3856
#define kPloytecBulkSize             4096
#define kPloytecInSize               5120

// Endpoints (from ploytec_defs.h)
#define kEpPcmOut   PLOYTEC_EP_PCM_OUT
#define kEpPcmIn    0x86
#define kEpMidiIn   0x83

PloytecEngine::PloytecEngine(uint16_t pid) {
    mIsBulk = false;
    mPacketSizeOut = mIsBulk ? kPloytecBulkSize : kPloytecInterruptSize;

    // --- Define Hardware Topology ---
    // The Xone:DB4 presents two interfaces:
    // Intf 0: Control + PCM Out + MIDI In
    // Intf 1: PCM In
    
    mProfile.interfaceCount = 2;
    mProfile.interfaceIndices[0] = 0;
    mProfile.interfaceIndices[1] = 1;
    
    // PCM Out (EP 0x05) lives on Interface 0
    mProfile.pcmOut.address = kEpPcmOut;
    mProfile.pcmOut.interfaceIndex = 0;
    
    // PCM In (EP 0x86) lives on Interface 1
    mProfile.pcmIn.address = kEpPcmIn;
    mProfile.pcmIn.interfaceIndex = 1;
    
    // MIDI In (EP 0x83) lives on Interface 0
    mProfile.midiIn.address = kEpMidiIn;
    mProfile.midiIn.interfaceIndex = 0;
}

bool PloytecEngine::Start() {
    auto* shm = GetSHM();
    if(!shm) return false;
    
    LogPloytec("PloytecEngine: Initializing...");

    // 1. Setup Shared Memory (device names already set by kext from USB descriptors)
    shm->audio.updateIntervalFrames = kPloytecSyncInterval;
    shm->audio.deviceFlags = (mIsBulk ? 1 : 0);
    shm->audio.framesPerPacket = kPloytecFramesPerPacket;  // 80 frames per logical packet
    shm->audio.samplesPerPacket = 10;  // 10 samples per USB sub-packet (8 sub-packets per logical packet)
    shm->audio.outputBytesPerFrame = 48; // Ploytec encoding: 48 bytes per frame
    shm->audio.inputBytesPerFrame = 64;  // Ploytec encoding: 64 bytes per frame
    
    // 2. Hardware Handshake Sequence
    ReadFirmwareVersion();
    GetHardwareFrameRate(); 
    
    if (!SetHardwareFrameRate(96000)) {
        LogPloytec("Failed to set Sample Rate");
        return false;
    }
    
    GetHardwareFrameRate(); 
    mBus->Sleep(50);
    
    /* Read status, set MODE5 bit, sign-extend, and write back */
    {
        uint8_t statusBuf[1] = {0};
        if (!mBus->VendorRequest(0xC0, 'I', 0, 0, statusBuf, 1)) {
            LogPloytec("Failed to read status for confirm");
            return false;
        }
        uint16_t wval = ploytec_confirm_wvalue(statusBuf[0]);
        WriteHardwareStatus(wval);
        LogPloytec("Status confirmed: read=0x%02X, wrote=0x%04X", statusBuf[0], wval);
    }
    ReadHardwareStatus();
    
    // Hardware is confirmed working and communicating
    shm->audio.hardwarePresent = true;

    // 3. Reset Engine State
    mHwSampleTime = 0;
    shm->audio.timestamp.sampleTime = 0;

    // 4. Pre-fill MIDI/UART sync pattern (0xFD) in entire output buffer
    // Each logical packet occupies kOzzyMaxPacketSize bytes
    // Each logical packet contains 8 USB sub-packets (482 or 512 bytes each)
    uint32_t subPacketSize = mIsBulk ? 512 : 482;
    uint32_t midiOffset = mIsBulk ? 480 : 432;
    
    for (uint32_t logicalPacket = 0; logicalPacket < kOzzyNumPackets; logicalPacket++) {
        uint32_t logicalPacketBase = logicalPacket * kOzzyMaxPacketSize;
        
        // Fill MIDI bytes in all 8 USB sub-packets within this logical packet
        for (uint32_t subPacket = 0; subPacket < 8; subPacket++) {
            uint32_t midiAddr = logicalPacketBase + (subPacket * subPacketSize) + midiOffset;
            shm->audio.outputBuffer[midiAddr] = 0xFD;
            shm->audio.outputBuffer[midiAddr + 1] = 0xFD;
        }
    }

    // 5. Kickstart USB transfers
    for(uint32_t i=0; i<2; i++) {
        SubmitPCMIn(i);
        SubmitPCMOut(i);
        SubmitMIDIIn(i);
    }
    
    // Driver is now fully initialized and ready for I/O
    shm->audio.driverReady = true;

    LogPloytec("PloytecEngine: Started.");
    return true;
}

void PloytecEngine::Stop() {
    auto* shm = GetSHM();
    if (!shm) return;
    
    // Clear flags immediately to signal HAL
    shm->audio.driverReady = false;
    shm->audio.hardwarePresent = false;
    
    LogPloytec("PloytecEngine: Stopped.");
}

void PloytecEngine::ProcessMIDIOutput(uint32_t packetIdx) {
    auto* shm = GetSHM();
    if (!shm) return;
    
    // Read pending MIDI bytes from the midiOut ring buffer (kernel space - volatile reads)
    uint32_t r = shm->midiOut.readIndex;
    uint32_t w = shm->midiOut.writeIndex;
    
    // Calculate MIDI byte positions for this packet (8 USB sub-packets per logical packet)
    uint32_t logicalPacket = packetIdx % kOzzyNumPackets;
    uint32_t logicalPacketBase = logicalPacket * kOzzyMaxPacketSize;
    uint32_t subPacketSize = mIsBulk ? 512 : 482;
    uint32_t midiOffset = mIsBulk ? 480 : 432;
    
    // Only use the first MIDI byte position in the first sub-packet
    // This limits MIDI output to ~1200 bytes/sec (9600 bps) at 96kHz, safely below 31.25 kbps
    uint32_t midiAddr = logicalPacketBase + midiOffset;
    
    // Write one MIDI byte if available, otherwise write 0xFD (sync/ignore byte)
    if (r != w) {
        shm->audio.outputBuffer[midiAddr] = shm->midiOut.buffer[r];
        r = (r + 1) & kOzzyMidiRingMask;
        
        // Update read index
        shm->midiOut.readIndex = r;
    } else {
        // No MIDI data available - write ignore byte
        shm->audio.outputBuffer[midiAddr] = 0xFD;
    }
}

void PloytecEngine::ProcessMIDIInput(uint32_t bytesReceived) {
    auto* shm = GetSHM();
    if (!shm || bytesReceived == 0) return;
    
    // Write received MIDI bytes to the midiIn ring buffer (kernel space - volatile writes)
    for (uint32_t i = 0; i < bytesReceived && i < sizeof(shm->midiInUSBBuffer); i++) {
        uint32_t w = shm->midiIn.writeIndex;
        uint32_t r = shm->midiIn.readIndex;
        uint32_t nextW = (w + 1) & kOzzyMidiRingMask;
        
        // Check if ring buffer has space
        if (nextW != r) {
            shm->midiIn.buffer[w] = shm->midiInUSBBuffer[i];
            shm->midiIn.writeIndex = nextW;
        }
    }
}

void PloytecEngine::OnPacketComplete(uint8_t pipeID, void* ctx, int status, uint32_t bytes) {
    if (!mBus) return;

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
             LogPloytec("Output Submission Failed for Index %d", nextIdx);
        }
    }
    else if (pipeID == kEpPcmIn) {
        // Use modulo to wrap around the full ring buffer (128 packets)
        uint32_t packetIdx = nextIdx % kOzzyNumPackets;
        uint32_t offset = packetIdx * kOzzyMaxPacketSize;
        uint8_t* buf = (uint8_t*)GetSHM()->audio.inputBuffer + offset;

        if (!mBus->SubmitUSBPacket(kEpPcmIn, buf, kPloytecInSize, (void*)(uintptr_t)nextIdx)) {
            LogPloytec("Input Submission Failed! (Idx: %d, Offset: %u)", nextIdx, offset);
        }
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

    mHwSampleTime += kPloytecFramesPerPacket;

    if ((mHwSampleTime % kPloytecSyncInterval) == 0) {
        volatile auto* ts = &shm->audio.timestamp;
        uint32_t seq = ts->sequence;
        ts->sequence = seq + 1;
        ts->sampleTime = mHwSampleTime;
        ts->hostTime = mBus->GetTime(); 
        ts->sequence = seq + 2;
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
    auto* shm = GetSHM();
    // Use modulo to wrap around the full ring buffer (128 packets)
    uint32_t pIdx = packetIdx % kOzzyNumPackets;
    
    // We assume 'inputBuffer' exists in shared memory mirroring outputBuffer.
    // If your struct is named differently, update this line.
    uint8_t* buffer = (uint8_t*)shm->audio.inputBuffer + (pIdx * kOzzyMaxPacketSize);
    
    // 0x86 & 0x80 = In, handled by KextBus logic
    mBus->SubmitUSBPacket(kEpPcmIn, buffer, kPloytecInSize, (void*)(uintptr_t)packetIdx);
}

void PloytecEngine::SubmitMIDIIn(uint32_t idx) {
    auto* shm = GetSHM();
    if (!mBus->SubmitUSBPacket(kEpMidiIn, (void*)shm->midiInUSBBuffer, sizeof(shm->midiInUSBBuffer), (void*)(uintptr_t)idx)) {
        LogPloytec("MIDI Input Submission Failed (idx: %u)", idx);
    }
}

// --------------------------------------------------------------------------
// Protocol Helpers
// --------------------------------------------------------------------------

bool PloytecEngine::ReadFirmwareVersion() {
    uint8_t buf[16] = {0};
    if (!mBus->VendorRequest(0xC0, PLOYTEC_CMD_FIRMWARE, 0, 0, buf, 0x0F)) return false;
    struct ploytec_firmware_version fw = ploytec_parse_firmware(buf);
    LogPloytec("Firmware: v1.%u.%u (ID:0x%02X)", fw.major, fw.minor, fw.chip_id);
    return true;
}

bool PloytecEngine::ReadHardwareStatus() {
    uint8_t buf[1] = {0};
    if (!mBus->VendorRequest(0xC0, 'I', 0, 0, buf, 1)) return false;
    uint8_t s = buf[0];
    
    LogPloytec("Status: [0x%02X] %s %s %s %s %s %s", s,
              (s & 0x80) ? "[HighSpeed]"   : "[FullSpeed]",
              (s & 0x20) ? "[Legacy/BCD1]" : "[Modern/BCD3]",
              (s & 0x10) ? "[Armed]"       : "[Disarmed]",
              (s & 0x04) ? "[Clock-Lock]"  : "[No-Lock]",
              (s & 0x02) ? "[Streaming]"   : "[Idle]",
              (s & 0x01) ? "[Stable]"      : "[Syncing]");
    return true;
}

bool PloytecEngine::GetHardwareFrameRate() {
    uint8_t buf[3] = {0};
    if (!mBus->VendorRequest(0xA2, 0x81, 0x0100, 0, buf, 3)) return false;
    uint32_t rate = ploytec_decode_rate(buf);
    LogPloytec("Current Rate: %u Hz", rate);
    return true;
}

bool PloytecEngine::SetHardwareFrameRate(uint32_t r) {
    LogPloytec("Setting Rate to %u Hz...", r);
    uint8_t buf[3];
    ploytec_encode_rate(r, buf);

    mBus->VendorRequest(PLOYTEC_CMD_SET_RATE_TYPE, PLOYTEC_CMD_SET_RATE_REQ, 0x0100, PLOYTEC_EP_RATE_IN, buf, 3);
    mBus->VendorRequest(PLOYTEC_CMD_SET_RATE_TYPE, PLOYTEC_CMD_SET_RATE_REQ, 0x0100, PLOYTEC_EP_RATE_OUT, buf, 3);
    mBus->VendorRequest(PLOYTEC_CMD_SET_RATE_TYPE, PLOYTEC_CMD_SET_RATE_REQ, 0x0100, PLOYTEC_EP_RATE_IN, buf, 3);
    mBus->VendorRequest(PLOYTEC_CMD_SET_RATE_TYPE, PLOYTEC_CMD_SET_RATE_REQ, 0x0100, PLOYTEC_EP_RATE_OUT, buf, 3);

    return mBus->VendorRequest(PLOYTEC_CMD_SET_RATE_TYPE, PLOYTEC_CMD_SET_RATE_REQ, 0x0100, PLOYTEC_EP_RATE_IN, buf, 3);
}

bool PloytecEngine::WriteHardwareStatus(uint16_t v) {
    LogPloytec("Writing Status: 0x%04X", v);
    return mBus->VendorRequest(0x40, 'I', v, 0, nullptr, 0);
}