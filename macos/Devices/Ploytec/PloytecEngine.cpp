#include "PloytecEngine.h"
#include "PloytecProtocol.h"
#include "../../../common/devices/ploytec/ploytec_handshake.h"
#include "../../Shared/OzzyLog.h"
#include <libkern/libkern.h>
#include <sys/errno.h>

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
    vsnprintf(line, sizeof(line), fmt, args);
    LogPloytec("%s", line);
}

// No clear_halt: the official macOS driver never clears the PCM endpoint halt
// (only the Windows driver does, see ploytec_handshake.c).
static const struct ploytec_host_ops kHostOps = { HostControl, nullptr, HostSleep, HostLog };

PloytecEngine::PloytecEngine(uint16_t pid) {

    // Xone:DB2 is 48 kHz only (official driver's per-device format table, XONE_DB2.kext plist).
    // Other devices keep running at 96 kHz.
    mSampleRate = (pid == PLOYTEC_PID_XONE_DB2) ? 48000 : 96000;

    // --- Define Hardware Topology ---
    // The Xone:DB4 presents two interfaces:
    // Intf 0: Control + PCM Out + MIDI In
    // Intf 1: PCM In
    
    mProfile.interfaceCount = PLOYTEC_NUM_INTERFACES;
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

/* The output packet layout follows the endpoint's wMaxPacketSize, not its transfer type,
 * like the official driver (AudioBulkIRP::useOut): <= 512 uses the bulk layout, larger (964)
 * uses the interrupt layout. DB2 fw 1.2.0 is an interrupt endpoint with wMaxPacketSize 512. */
void PloytecEngine::SetOutEndpoint(bool isBulk, uint16_t maxPacketSize) {
    mOutLayout = ploytec_out_layout_for_maxpacket(maxPacketSize);
    mPacketSizeOut = ploytec_out_packet_size(mOutLayout);
    LogPloytec("transport: %s, wMaxPacketSize %u (%s layout, out packet: %u bytes)",
               isBulk ? "bulk" : "interrupt", maxPacketSize,
               mOutLayout == PLOYTEC_OUT_LAYOUT_BULK ? "bulk" : "interrupt", mPacketSizeOut);
}

bool PloytecEngine::Start() {
    auto* shm = GetSHM();
    if(!shm) return false;
    
    // 1. Setup Shared Memory (device names already set by kext from USB descriptors)
    shm->audio.deviceFlags = (mOutLayout == PLOYTEC_OUT_LAYOUT_BULK ? 1 : 0);
    shm->audio.sampleRate = mSampleRate;
    shm->audio.framesPerPacket = PLOYTEC_FRAMES_PER_PKT;  // 80 frames per logical packet
    shm->audio.samplesPerPacket = 10;  // 10 samples per USB sub-packet
    shm->audio.outputBytesPerFrame = PLOYTEC_OUT_FRAME_SIZE;
    shm->audio.inputBytesPerFrame = PLOYTEC_IN_FRAME_SIZE;
    
    // 2. Hardware Handshake Sequence
    if (ploytec_handshake(&kHostOps, mBus, mSampleRate, nullptr) < 0)
        return false;
    
    // Hardware is confirmed working and communicating
    shm->audio.hardwarePresent = true;

    // 3. Reset Engine State
    mHwSampleTime = 0;
    shm->audio.timestamp.sampleTime = 0;

    // 4. Fill every logical packet (kOzzyMaxPacketSize stride) with silence and idle MIDI bytes
    for (uint32_t logicalPacket = 0; logicalPacket < kOzzyNumPackets; logicalPacket++) {
        ploytec_out_init_packet(mOutLayout, (uint8_t*)shm->audio.outputBuffer + logicalPacket * kOzzyMaxPacketSize);
    }

    // 5. Kickstart USB transfers
    for(uint32_t i=0; i<2; i++) {
        SubmitPCMIn(i);
        SubmitPCMOut(i);
        SubmitMIDIIn(i);
    }
    
    // Driver is now fully initialized and ready for I/O
    shm->audio.driverReady = true;

    return true;
}

void PloytecEngine::Stop() {
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
    
    // Read pending MIDI bytes from the midiOut ring buffer (kernel space - volatile reads)
    uint32_t r = shm->midiOut.readIndex;
    uint32_t w = shm->midiOut.writeIndex;
    
    uint8_t* packet = (uint8_t*)shm->audio.outputBuffer + (packetIdx % kOzzyNumPackets) * kOzzyMaxPacketSize;
    uint8_t byte = PLOYTEC_MIDI_IDLE_BYTE;
    if (r != w) {
        byte = shm->midiOut.buffer[r];
        shm->midiOut.readIndex = (r + 1) & kOzzyMidiRingMask;
    }
    ploytec_out_set_midi(mOutLayout, packet, byte);
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
             LogPloytec("output submission failed (idx: %d)", nextIdx);
        }
    }
    else if (pipeID == kEpPcmIn) {
        // Use modulo to wrap around the full ring buffer (128 packets)
        uint32_t packetIdx = nextIdx % kOzzyNumPackets;
        uint32_t offset = packetIdx * kOzzyMaxPacketSize;
        uint8_t* buf = (uint8_t*)GetSHM()->audio.inputBuffer + offset;

        if (!mBus->SubmitUSBPacket(kEpPcmIn, buf, PLOYTEC_IN_PKT_SIZE, (void*)(uintptr_t)nextIdx)) {
            LogPloytec("input submission failed (idx: %d, offset: %u)", nextIdx, offset);
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

    mHwSampleTime += PLOYTEC_FRAMES_PER_PKT;

    if ((mHwSampleTime % (PLOYTEC_FRAMES_PER_PKT * kOzzyTimestampPackets)) == 0) {
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
    mBus->SubmitUSBPacket(kEpPcmIn, buffer, PLOYTEC_IN_PKT_SIZE, (void*)(uintptr_t)packetIdx);
}

void PloytecEngine::SubmitMIDIIn(uint32_t idx) {
    auto* shm = GetSHM();
    if (!mBus->SubmitUSBPacket(kEpMidiIn, (void*)shm->midiInUSBBuffer, sizeof(shm->midiInUSBBuffer), (void*)(uintptr_t)idx)) {
        LogPloytec("MIDI input submission failed (idx: %u)", idx);
    }
}
