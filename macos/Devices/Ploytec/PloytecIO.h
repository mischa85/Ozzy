#ifndef PloytecIO_h
#define PloytecIO_h

#include "../../OzzyCore/IOzzyBus.h"
#include "../../Shared/OzzySharedData.h"
#include "../../../common/devices/ploytec/ploytec_midi.h"

/*
 * PCM in and MIDI in, the same on every Ploytec device (PloytecEngine, PloytecIsocEngine).
 */

// One PCM IN transfer: 80 frames into logical packet @packetIdx of inputBuffer
// (kOzzyMaxPacketSize stride), where the HAL's reader expects it
static inline bool PloytecSubmitPCMIn(IOzzyBus* bus, volatile OzzySharedMemory* shm, uint8_t address, uint32_t packetIdx) {
    uint8_t* buffer = (uint8_t*)shm->audio.inputBuffer + (packetIdx % kOzzyNumPackets) * kOzzyMaxPacketSize;
    return bus->SubmitUSBPacket(address, buffer, PLOYTEC_IN_PKT_SIZE, (void*)(uintptr_t)packetIdx);
}

// The MIDI bytes of one MIDI IN transfer (ploytec_midi.h) into the midiIn ring
static inline void PloytecMidiIn(const struct ploytec_midi* midi, volatile OzzySharedMemory* shm,
                                 const uint8_t* transfer, uint32_t bytes) {
    uint8_t bytesOut[PLOYTEC_MIDI_IN_MAX_SLOTS];
    uint32_t count = ploytec_midi_in(midi, transfer, bytes, bytesOut);
    OzzyMidiRingPush(&shm->midiIn, bytesOut, count);
}

#endif
