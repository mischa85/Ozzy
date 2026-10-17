/* SPDX-License-Identifier: MIT */
/*
 * What every Ploytec engine (ploytec_engine, ploytec_isoc_engine) does the same way: the
 * handshake's host services over the bus, PCM in and MIDI in.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef PLOYTEC_IO_H
#define PLOYTEC_IO_H

#include "../../core/ozzy_engine.h"
#include "../../../common/devices/ploytec/ploytec_midi.h"
#include "../../../common/devices/ploytec/ploytec_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Host services for the shared handshake (common/devices/ploytec/ploytec_handshake.c); the
 * context is the engine's struct ozzy_bus */
extern const struct ploytec_host_ops ploytec_bus_host_ops;

/* One PCM IN transfer: 80 frames into logical packet @idx of input_buffer
 * (OZZY_MAX_PACKET_SIZE stride), where the HAL's reader expects it */
static inline bool ploytec_submit_pcm_in(const struct ozzy_bus *bus, volatile struct ozzy_shared_memory *shm,
					 uint8_t address, uint32_t idx)
{
	uint8_t *buffer = (uint8_t *)shm->audio.input_buffer + (idx % OZZY_NUM_PACKETS) * OZZY_MAX_PACKET_SIZE;

	return ozzy_bus_submit(bus, address, buffer, PLOYTEC_IN_PKT_SIZE, (void *)(uintptr_t)idx);
}

/* The MIDI bytes of one MIDI IN transfer (ploytec_midi.h) into the midi_in ring */
static inline void ploytec_midi_in_to_ring(const struct ploytec_midi *midi, volatile struct ozzy_shared_memory *shm,
					   const uint8_t *transfer, uint32_t bytes)
{
	uint8_t out[PLOYTEC_MIDI_IN_MAX_SLOTS];
	uint32_t count = ploytec_midi_in(midi, transfer, bytes, out);

	ozzy_midi_ring_push(&shm->midi_in, out, count);
}

#ifdef __cplusplus
}
#endif

#endif /* PLOYTEC_IO_H */
