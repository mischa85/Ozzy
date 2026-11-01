/* SPDX-License-Identifier: MIT */
/*
 * Isochronous PCM in for any device that sends plain interleaved frames (S24_3LE) in its
 * packets, whatever number of them each microframe (the Tascam US-366: 6 per microframe at
 * 48 kHz, 12 at 96 kHz). An engine owns one and forwards its isochronous completions.
 *
 * Every packet asks for the endpoint's bytes per interval; what arrives is copied, in the
 * completion path, either into a plain frame ring in shared memory (the HAL reads it by sample
 * time, OZZY_DEVICE_FLAG_PLAIN_INPUT) or to the engine (@deliver: the attached ring, with the
 * input routing). Frames count from 0 at start, as the output's sample time does, so frame f
 * of the ring is the one the device sent at about sample time f. A failed packet leaves the
 * nominal frames of silence, so the input keeps the clock.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_ISOC_INPUT_H
#define OZZY_ISOC_INPUT_H

#include "ozzy_bus.h"
#include "../shared/ozzy_shared_data.h"
#include "../../common/isoc_feedback.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Transfers in flight, each ISOC_IN_UFRAMES microframes long: 8 ms queued, a completion every
 * 2 ms (the official Windows driver of the US-366 also sends 2 ms transfers, up to 9 of them)
 */
#define ISOC_IN_UFRAMES    16
#define ISOC_IN_TRANSFERS  4

/*
 * struct ozzy_isoc_input_config
 * @frame_bytes:   bytes per plain frame (channels * 3)
 * @in_max_packet: bytes per service interval (each packet asks for this much)
 * @xfer:          one buffer per transfer, @xfer_size bytes each (at least
 *                 ozzy_isoc_input_xfer_bytes)
 * @ring:          the plain frame ring (@ring_frames frames) the frames go to, or NULL:
 *                 @deliver gets them
 * @deliver:       @frames frames from @src (NULL: silence), in order; @done after each
 *                 transfer's (optional)
 */
struct ozzy_isoc_input_config {
	uint32_t rate;
	uint32_t frame_bytes;
	uint8_t in_addr;
	uint16_t in_max_packet;
	uint8_t in_interval;
	uint8_t *xfer[ISOC_IN_TRANSFERS];
	uint32_t xfer_size;
	uint8_t *ring;
	uint32_t ring_frames;
	void (*deliver)(void *owner, const uint8_t *src, uint32_t frames);
	void (*done)(void *owner);
	void *owner;
};

struct ozzy_isoc_input_xfer {
	struct ozzy_isoc_packet packets[OZZY_MAX_ISOC_PACKETS];
	uint32_t count;
};

struct ozzy_isoc_input {
	const struct ozzy_bus *bus;
	struct ozzy_isoc_input_config config;
	volatile bool running;

	struct isoc_fb nominal;    /* the frames a packet should have had (no feedback: the rate) */
	uint32_t uframes;          /* microframes per packet */
	uint64_t in_frame;         /* frames received (the next one's ring position) */
	bool logged;
	struct ozzy_isoc_input_xfer in[ISOC_IN_TRANSFERS];
};

/* Packets per transfer on an endpoint of bInterval @interval */
static inline uint32_t ozzy_isoc_input_packets(uint8_t interval)
{
	uint32_t n = ISOC_IN_UFRAMES / isoc_fb_interval_uframes(interval);

	return n + (n == 0);
}

/* Bytes one transfer needs: every packet the endpoint's bytes per interval */
static inline uint32_t ozzy_isoc_input_xfer_bytes(uint16_t max_packet, uint8_t interval)
{
	return ozzy_isoc_input_packets(interval) * max_packet;
}

/*
 * Frames behind the clock the input is complete at @rate (the HAL's input safety offset): one
 * transfer's, the most it can carry, plus the jitter floor. A guess until measured on hardware.
 */
static inline uint32_t ozzy_isoc_input_safety_offset(uint32_t rate)
{
	return isoc_fb_max_frames(rate, ISOC_IN_UFRAMES) +
	       (uint32_t)(((uint64_t)rate * 1000 + 999999) / 1000000);
}

/* Reset the frame count (and clear the ring), submit the transfers */
bool ozzy_isoc_input_start(struct ozzy_isoc_input *in, const struct ozzy_bus *bus,
			   const struct ozzy_isoc_input_config *config);

static inline void ozzy_isoc_input_stop(struct ozzy_isoc_input *in)
{
	in->running = false;
}

/* An isochronous completion; false if it isn't this input's pipe */
bool ozzy_isoc_input_complete(struct ozzy_isoc_input *in, uint8_t pipe, void *xfer, int status,
			      struct ozzy_isoc_packet *packets, uint32_t count);

#ifdef __cplusplus
}
#endif

#endif /* OZZY_ISOC_INPUT_H */
