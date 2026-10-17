/* SPDX-License-Identifier: MIT */
/*
 * Isochronous PCM out for any device whose output takes plain interleaved frames, paced by a
 * feedback endpoint (common/isoc_feedback.h, the same pacing as Linux's core). An engine owns
 * one and forwards its isochronous completions.
 *
 * The HAL writes frames into a ring in shared memory (OZZY_DEVICE_FLAG_PLAIN_OUTPUT), and each
 * microframe's packet points straight into it (zero copy; a packet that runs past the end
 * wraps). The output clock is the device's: the frames of completed transfers advance the
 * sample time, and the timestamp anchors use the completion time of the microframe that
 * reaches each one.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_ISOC_OUTPUT_H
#define OZZY_ISOC_OUTPUT_H

#include "ozzy_bus.h"
#include "../shared/ozzy_shared_data.h"
#include "../../common/isoc_feedback.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * struct ozzy_isoc_output_config
 * @frame_bytes:    bytes per plain frame (channels * 3)
 * @ring_frames:    frames in the output ring
 * @anchor_frames:  timestamp anchor spacing (the HAL's zero timestamp period)
 * @out_max_packet: bytes per service interval
 * @fb_addr:        0: no feedback endpoint, nominal rate
 * @ring:           the plain frame ring OUT packets point into (the HAL writes it on macOS)
 * @xfer:           instead of @ring, one buffer per OUT transfer (@xfer_size bytes each), its
 *                  packets one after the other from the start; @fill writes each before it
 *                  goes out (platforms whose transfers can't wrap a ring: Linux)
 * @fb_buf:         the feedback transfers (@fb_size bytes)
 * @fill:           optional: called before each OUT transfer to write its @frames frames into
 *                  @dst, from position @first wrapping at @dst_frames; without it the HAL has
 *                  written them (macOS)
 */
struct ozzy_isoc_output_config {
	uint32_t rate;
	uint32_t frame_bytes;
	uint32_t ring_frames;
	uint64_t anchor_frames;
	uint8_t out_addr;
	uint16_t out_max_packet;
	uint8_t out_interval;
	uint8_t fb_addr;
	uint16_t fb_max_packet;
	uint8_t fb_interval;
	struct isoc_fb_config feedback;
	uint8_t *ring;
	uint8_t *xfer[ISOC_OUT_TRANSFERS];
	uint32_t xfer_size;
	uint8_t *fb_buf;
	uint32_t fb_size;
	void (*fill)(void *owner, uint8_t *dst, uint32_t dst_frames, uint64_t first, uint32_t frames);
	void *owner;
};

struct ozzy_isoc_output_xfer {
	struct ozzy_isoc_packet packets[OZZY_MAX_ISOC_PACKETS];
	uint32_t frames[OZZY_MAX_ISOC_PACKETS];
	uint32_t count;
};

struct ozzy_isoc_output {
	const struct ozzy_bus *bus;
	volatile struct ozzy_shared_memory *shm;
	struct ozzy_isoc_output_config config;
	volatile bool running;

	struct isoc_fb fb;
	uint32_t out_uframes;      /* microframes per OUT packet */
	uint32_t out_max_frames;   /* frames that fit one OUT packet */
	uint64_t out_frame;        /* next frame to send (ring position) */
	uint64_t played_frames;    /* frames of completed transfers (the device's sample time) */
	uint64_t next_anchor;      /* sample time of the next timestamp anchor */
	bool fb_logged;
	struct ozzy_isoc_output_xfer out[ISOC_OUT_TRANSFERS];
	struct ozzy_isoc_output_xfer fb_xfer[ISOC_FB_TRANSFERS];
};

/* Frames the output reads ahead of the clock at @rate (the HAL's safety offset) */
static inline uint32_t ozzy_isoc_output_safety_offset(uint32_t rate)
{
	return ISOC_OUT_TRANSFERS * isoc_fb_max_frames(rate, ISOC_TRANSFER_UFRAMES);
}

/* Clear the ring, reset the clock, submit the feedback and OUT transfers */
bool ozzy_isoc_output_start(struct ozzy_isoc_output *out, const struct ozzy_bus *bus,
			    volatile struct ozzy_shared_memory *shm, const struct ozzy_isoc_output_config *config);

static inline void ozzy_isoc_output_stop(struct ozzy_isoc_output *out)
{
	out->running = false;
}

/* An isochronous completion; false if it isn't one of this output's pipes */
bool ozzy_isoc_output_complete(struct ozzy_isoc_output *out, uint8_t pipe, void *xfer, int status,
			       struct ozzy_isoc_packet *packets, uint32_t count);

#ifdef __cplusplus
}
#endif

#endif /* OZZY_ISOC_OUTPUT_H */
