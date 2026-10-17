#ifndef PloytecIO_h
#define PloytecIO_h

#include "../../core/IOzzyBus.h"
#include "../../shared/ozzy_shared_data.h"
#include "../../../common/devices/ploytec/ploytec_midi.h"

/*
 * PCM in and MIDI in, the same on every Ploytec device (PloytecEngine, PloytecIsocEngine).
 */

// One PCM IN transfer: 80 frames into logical packet @packetIdx of inputBuffer
// (OZZY_MAX_PACKET_SIZE stride), where the HAL's reader expects it
static inline bool PloytecSubmitPCMIn(IOzzyBus* bus, volatile ozzy_shared_memory* shm, uint8_t address, uint32_t packetIdx) {
    uint8_t* buffer = (uint8_t*)shm->audio.input_buffer + (packetIdx % OZZY_NUM_PACKETS) * OZZY_MAX_PACKET_SIZE;
    return bus->SubmitUSBPacket(address, buffer, PLOYTEC_IN_PKT_SIZE, (void*)(uintptr_t)packetIdx);
}

// The MIDI bytes of one MIDI IN transfer (ploytec_midi.h) into the midiIn ring
static inline void PloytecMidiIn(const struct ploytec_midi* midi, volatile ozzy_shared_memory* shm,
                                 const uint8_t* transfer, uint32_t bytes) {
    uint8_t bytesOut[PLOYTEC_MIDI_IN_MAX_SLOTS];
    uint32_t count = ploytec_midi_in(midi, transfer, bytes, bytesOut);
    ozzy_midi_ring_push(&shm->midi_in, bytesOut, count);
}

#endif
