/* SPDX-License-Identifier: MIT */
/*
 * Ozzy Routing: stereo pairs between the host's channels and the device's
 *
 * The official driver's routing (Tascam US-144MKII, US-600, US-200; .claude/re/device_settings.md)
 * moves whole stereo pairs on the host side, with no request to the device:
 *   - output: device output pair k plays host playback pair src[k] (pcmTo24LSB_US144_*)
 *   - input:  host capture pair k carries device input pair src[k] (_postprocessUS144_* /
 *             _postprocessUS600_*)
 * A pair may be used more than once. The default, src[k] = k, routes nothing.
 *
 * Pure C with no platform dependencies -- usable in both kernel space
 * and userspace on any platform.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_ROUTE_H
#define OZZY_ROUTE_H

#if defined(__KERNEL__) && defined(__linux__)
#include <linux/types.h>
#include <linux/string.h>
#else
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define OZZY_ROUTE_MAX_PAIRS 8

/*
 * struct ozzy_route - One direction's routing.
 * @pairs:    stereo pairs in a frame
 * @src:      the source pair of each pair
 * @identity: src[k] == k for every pair (nothing to do)
 */
struct ozzy_route {
	uint8_t pairs;
	uint8_t src[OZZY_ROUTE_MAX_PAIRS];
	bool identity;
};

/*
 * ozzy_route_set - Routing for @channels channels with source pairs @src (NULL: none).
 * Sources out of range fall back to the pair itself.
 */
static inline void ozzy_route_set(struct ozzy_route *r, unsigned int channels, const uint8_t *src)
{
	unsigned int k;

	r->pairs = (uint8_t)(channels / 2 < OZZY_ROUTE_MAX_PAIRS ? channels / 2 : OZZY_ROUTE_MAX_PAIRS);
	r->identity = true;
	for (k = 0; k < r->pairs; k++) {
		r->src[k] = (uint8_t)((src && src[k] < r->pairs) ? src[k] : k);
		if (r->src[k] != k)
			r->identity = false;
	}
}

/*
 * ozzy_route_frame - Copy one frame from @src to @dst (separate buffers) with its pairs
 * routed. @pair_bytes: bytes per stereo pair (2 samples).
 */
static inline void ozzy_route_frame(uint8_t *dst, const uint8_t *src, const struct ozzy_route *r,
				    unsigned int pair_bytes)
{
	unsigned int k;

	for (k = 0; k < r->pairs; k++)
		memcpy(dst + k * pair_bytes, src + r->src[k] * pair_bytes, pair_bytes);
}

/*
 * ozzy_route_frames - Route @frames whole frames in place. @pair_bytes at most 8
 * (two 32-bit samples).
 */
static inline void ozzy_route_frames(uint8_t *buf, unsigned int frames, const struct ozzy_route *r,
				     unsigned int pair_bytes)
{
	uint8_t tmp[OZZY_ROUTE_MAX_PAIRS * 8];
	unsigned int frame_bytes = r->pairs * pair_bytes;
	unsigned int f;

	if (r->identity || pair_bytes > 8)
		return;
	for (f = 0; f < frames; f++, buf += frame_bytes) {
		memcpy(tmp, buf, frame_bytes);
		ozzy_route_frame(buf, tmp, r, pair_bytes);
	}
}

#ifdef __cplusplus
}
#endif

#endif /* OZZY_ROUTE_H */
