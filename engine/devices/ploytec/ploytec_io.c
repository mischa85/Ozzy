/* SPDX-License-Identifier: MIT */
/*
 * What every Ploytec engine does the same way (ploytec_io.h).
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "ploytec_io.h"

void ploytec_decode_pcm_in(struct ozzy_engine *e, ploytec_frame_fn decode, uint32_t frames, uint32_t t, uint32_t bytes)
{
	const struct ozzy_pcm_ring *ring = ozzy_engine_take_pcm_ring(e, OZZY_PCM_IN);
	const uint8_t *packet;
	uint32_t frame_bytes, got, r, f;

	if (!ring || !decode)
		return;
	packet = ozzy_engine_pcm_in_slot(e, t);
	frame_bytes = ring->channels * 3;
	got = bytes / PLOYTEC_IN_FRAME_SIZE;
	r = ozzy_mod_u64(e->pcm_pos[OZZY_PCM_IN], ring->frames);
	for (f = 0; f < frames; f++) {
		uint8_t *dst = ring->base + r * frame_bytes;
		uint8_t decoded[PLOYTEC_MAX_IN_CHANNELS * 3];

		if (f >= got) {
			memset(dst, 0, frame_bytes);
		} else if (e->in_route.identity) {
			decode(dst, packet + f * PLOYTEC_IN_FRAME_SIZE);
		} else {
			/* Input routing (US-144MKII, US-600): decode, then move the pairs */
			decode(decoded, packet + f * PLOYTEC_IN_FRAME_SIZE);
			ozzy_route_frame(dst, decoded, &e->in_route, 2 * 3);
		}
		if (++r == ring->frames)
			r = 0;
	}

	e->pcm_pos[OZZY_PCM_IN] += frames;
	ozzy_bus_pcm_progress(e->bus, OZZY_PCM_IN, e->pcm_pos[OZZY_PCM_IN]);
}
