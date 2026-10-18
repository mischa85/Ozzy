/* SPDX-License-Identifier: MIT */
/*
 * What every Ploytec engine (ploytec_engine, ploytec_isoc_engine) does the same way: the
 * handshake's host services over the bus, PCM in and MIDI in (ploytec_io.c).
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef PLOYTEC_IO_H
#define PLOYTEC_IO_H

#include "../../core/ozzy_engine.h"
#include "../../../common/devices/ploytec/ploytec_midi.h"
#include "../../../common/devices/ploytec/ploytec_protocol.h"
#include "../../../common/devices/ploytec/ploytec_codec.h"
#include "../../../common/devices/ploytec/ploytec_handshake.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Host services for the shared handshake (common/devices/ploytec/ploytec_handshake.c), without
 * clear_halt; the context is the engine's struct ozzy_bus */
extern const struct ploytec_host_ops ploytec_bus_host_ops;

/* What the handshake read, for ozzy_info: the firmware ('V', shown as 1.<major>.<minor>, as
 * the official tools do), the rate the device booted at and the input selector byte */
static inline void ploytec_report(struct ozzy_engine *e, const struct ploytec_handshake_info *hs)
{
	e->report.firmware_valid = true;
	e->report.firmware_chip = hs->firmware.chip_id;
	e->report.firmware[0] = 1;
	e->report.firmware[1] = hs->firmware.major;
	e->report.firmware[2] = hs->firmware.minor;
	e->report.boot_rate = hs->boot_rate;
	e->report.input_selector_valid = true;
	e->report.input_selector = hs->status;
}

/*
 * PCM transfers: any whole number of sub-packets, at the engine's places in turn
 * (ozzy_engine_pcm_layout: in the shared memory's rings end to end, where the HAL reads and
 * writes them by sample time; or the platform's own buffers). Transfer indices wrap at
 * ploytec_pcm_wrap, a multiple of the places, so a transfer keeps its place in the ring (2^32
 * is one only when the places are a power of two).
 */
static inline uint32_t ploytec_pcm_wrap(uint32_t places)
{
	return (places ? places : 1) << 20;
}

/* The most places a ring has (one 482-byte sub-packet per transfer), shifted, must fit */
typedef char ploytec_pcm_wrap_fits[((uint64_t)(OZZY_PCM_RING_BYTES / PLOYTEC_INT_OUT_SUBPKT_SIZE) << 20) <=
				   0xFFFFFFFFu ? 1 : -1];

/* Transfer @t - 1, wrapped */
static inline uint32_t ploytec_pcm_prev(uint32_t t, uint32_t places)
{
	return t ? t - 1 : ploytec_pcm_wrap(places) - 1;
}

/* Transfer @t + @n, wrapped */
static inline uint32_t ploytec_pcm_next(uint32_t t, uint32_t n, uint32_t places)
{
	return (t + n) % ploytec_pcm_wrap(places);
}

/* Bytes of a PCM out transfer of @frames frames (frames / 10 sub-packets) */
static inline uint32_t ploytec_out_xfer_bytes(enum ploytec_out_layout layout, uint32_t frames)
{
	return frames / PLOYTEC_FRAMES_PER_OUT_SUBPKT * ploytec_out_subpkt_size(layout);
}

/* Bytes of a PCM in transfer of @frames frames (frames / 8 sub-packets of 512 bytes) */
static inline uint32_t ploytec_in_xfer_bytes(uint32_t frames)
{
	return frames * PLOYTEC_IN_FRAME_SIZE;
}

/* One PCM IN transfer of @frames frames, at transfer @t's place */
static inline bool ploytec_submit_pcm_in(const struct ozzy_engine *e, uint8_t address, uint32_t frames, uint32_t t)
{
	return ozzy_bus_submit(e->bus, address, ozzy_engine_pcm_in_slot(e, t), ploytec_in_xfer_bytes(frames),
			       (void *)(uintptr_t)t);
}

/*
 * MIDI IN: PLOYTEC_MIDI_IN_XFERS reads of the framing's transfer size (9 bytes on the Xones,
 * as Linux reads them), each into its own OZZY_DMA_ALIGN slot of the MIDI IN memory
 */
#define PLOYTEC_MIDI_IN_XFERS   4
#define PLOYTEC_MIDI_IN_SLOT    OZZY_DMA_ALIGN   /* >= PLOYTEC_MIDI_XFER_MAX */

static inline uint8_t *ploytec_midi_in_slot(const struct ozzy_engine *e, uint32_t slot)
{
	return e->mem.midi_in + slot * PLOYTEC_MIDI_IN_SLOT;
}

static inline bool ploytec_submit_midi_in(const struct ozzy_engine *e, uint8_t address, const struct ploytec_midi *midi,
					  uint32_t slot)
{
	return ozzy_bus_submit(e->bus, address, ploytec_midi_in_slot(e, slot), midi->xfer_size, (void *)(uintptr_t)slot);
}

/* The MIDI bytes of one MIDI IN transfer (ploytec_midi.h) into the midi_in ring */
static inline void ploytec_midi_in_to_ring(const struct ploytec_midi *midi, volatile struct ozzy_shared_memory *shm,
					   const uint8_t *transfer, uint32_t bytes)
{
	uint8_t out[PLOYTEC_MIDI_IN_MAX_SLOTS];
	uint32_t count = ploytec_midi_in(midi, transfer, bytes, out);

	ozzy_midi_ring_push(&shm->midi_in, out, count);
}

/*
 * PCM in into the attached ring (ploytec_io.c): the @frames frames of completed IN transfer @t,
 * decoded straight from the shared memory into the ring with the input routing. A short (or
 * failed, @bytes 0) transfer leaves silence, so the ring keeps the device's clock.
 */
void ploytec_decode_pcm_in(struct ozzy_engine *e, ploytec_frame_fn decode, uint32_t frames, uint32_t t, uint32_t bytes);

#ifdef __cplusplus
}
#endif

#endif /* PLOYTEC_IO_H */
