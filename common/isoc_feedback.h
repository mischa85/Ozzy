/* SPDX-License-Identifier: MIT */
/*
 * Isochronous Output Pacing from a Feedback Endpoint
 *
 * With isochronous PCM out the host decides how many frames go into each
 * (micro)frame's packet, and the device reports through an isochronous IN
 * feedback endpoint how fast it really plays. This keeps a ring of frame counts,
 * one per High Speed microframe: the feedback side fills it, the output side
 * plays it back, so the host sends exactly as many frames as the device
 * consumes, one ring lap (16 ms) later. Where there is no valid feedback the
 * output runs at the nominal rate.
 *
 * Feedback formats:
 *
 *   ISOC_FB_COUNT  - byte 0 = frames the device played in the packet's interval.
 *                    The Ploytec bulk in / isoc out devices (Akai EIE Pro, 3-byte
 *                    packets; PGFramePatternObserver in the official kext).
 *   ISOC_FB_Q16_16 - frames per microframe, 16.16 little-endian in 4 bytes (the
 *                    USB Audio High Speed format).
 *
 * Packets of the wrong length are counted. Past @max_errors without a clean ring
 * lap in between, feedback is ignored (the official Ploytec driver stops the
 * stream after 40 there; this keeps playing at the nominal rate).
 *
 * The two sides share only the ring, with no lock: each side owns its own
 * index, and entries are single bytes. High Speed only.
 *
 * Pure C with no platform dependencies -- usable in both kernel space
 * and userspace on any platform.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_ISOC_FEEDBACK_H
#define OZZY_ISOC_FEEDBACK_H

#if defined(__KERNEL__) && defined(__linux__)
#include <linux/types.h>
#include <linux/compiler.h>
#define ISOC_FB_LOAD(x)      READ_ONCE(x)
#define ISOC_FB_STORE(x, v)  WRITE_ONCE(x, v)
#else
#include <stdint.h>
#include <stdbool.h>
#define ISOC_FB_LOAD(x)      __atomic_load_n(&(x), __ATOMIC_RELAXED)
#define ISOC_FB_STORE(x, v)  __atomic_store_n(&(x), (v), __ATOMIC_RELAXED)
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define ISOC_FB_UFRAMES_PER_SEC  8000   /* High Speed microframes */
#define ISOC_FB_RING             128    /* microframes (16 ms), power of two */
#define ISOC_FB_MAX_UFRAMES      8      /* feedback every 1 ms at most (bInterval 4) */

enum isoc_fb_format {
	ISOC_FB_NONE,      /* no feedback endpoint: nominal rate */
	ISOC_FB_COUNT,     /* byte 0 = frames in the packet's interval (Ploytec) */
	ISOC_FB_Q16_16,    /* frames per microframe, 16.16 LE (USB Audio High Speed) */
};

/*
 * struct isoc_fb_config - How a device reports feedback (static, per device).
 * @format:      one of isoc_fb_format
 * @packet_size: valid feedback packet length in bytes
 * @max_errors:  wrong-length packets tolerated without a clean lap (0 = no limit)
 */
struct isoc_fb_config {
	uint8_t format;
	uint8_t packet_size;
	uint16_t max_errors;
};

/*
 * struct isoc_fb - Output pacing for one stream. Set up with isoc_fb_init()
 * whenever the rate changes, while no URBs run.
 */
struct isoc_fb {
	/* Frames per microframe from the feedback, 0 = none (use the nominal rate) */
	uint8_t ring[ISOC_FB_RING];

	/* Output side */
	uint32_t rd;            /* next ring entry to play */
	uint32_t rate;          /* sample rate */
	uint32_t nominal_acc;   /* nominal frames per microframe, error accumulator (< 8000) */

	/* Feedback side */
	struct isoc_fb_config cfg;
	uint32_t wr;            /* next ring entry to fill */
	uint32_t uframes;       /* microframes per feedback packet, 0 = feedback not used */
	uint32_t lo, hi;        /* accepted frames per packet (COUNT) or per microframe (Q16_16) */
	uint32_t q_acc;         /* Q16_16: fractional frames carried to the next microframe */
	uint32_t errors;        /* wrong-length packets since the last clean lap */
	uint32_t lap_errors;    /* wrong-length packets in this lap */
	bool broken;            /* errors passed the limit: feedback is ignored */
};

/*
 * isoc_fb_interval_uframes - Microframes between packets of a High Speed
 * isochronous endpoint with this bInterval (2^(bInterval-1)).
 */
static inline uint32_t isoc_fb_interval_uframes(uint8_t b_interval)
{
	return 1u << ((b_interval ? b_interval : 1) - 1);
}

/*
 * isoc_fb_max_frames - Most frames isoc_fb_out_frames() returns for @uframes
 * microframes at @rate (to size packets).
 */
static inline uint32_t isoc_fb_max_frames(uint32_t rate, uint32_t uframes)
{
	return uframes * ((rate + ISOC_FB_UFRAMES_PER_SEC - 1) / ISOC_FB_UFRAMES_PER_SEC + 2);
}

/*
 * isoc_fb_init - Set up output pacing at @rate Hz.
 * @cfg:         the device's feedback format (format ISOC_FB_NONE: nominal rate)
 * @fb_interval: bInterval of the feedback endpoint
 * Returns false if the feedback interval is longer than 1 ms, which this doesn't
 * handle (neither does the official Ploytec driver); the output then runs at
 * the nominal rate.
 */
static inline bool isoc_fb_init(struct isoc_fb *fb, uint32_t rate,
				const struct isoc_fb_config *cfg, uint8_t fb_interval)
{
	uint32_t u = cfg->format != ISOC_FB_NONE ? isoc_fb_interval_uframes(fb_interval) : 0;
	uint32_t per = cfg->format == ISOC_FB_COUNT ? u : 1;   /* microframes one value covers */
	uint32_t tol, i;
	bool ok = u <= ISOC_FB_MAX_UFRAMES;

	for (i = 0; i < ISOC_FB_RING; i++)
		ISOC_FB_STORE(fb->ring[i], 0);
	fb->rd = 0;
	fb->rate = rate;
	fb->nominal_acc = 0;
	fb->cfg = *cfg;
	fb->wr = 0;
	fb->uframes = ok ? u : 0;
	fb->q_acc = 0;
	fb->errors = 0;
	fb->lap_errors = 0;
	fb->broken = false;

	/* The Ploytec kext accepts nominal +-1 per microframe and +-2 per millisecond */
	tol = 1 + (per >= ISOC_FB_MAX_UFRAMES);
	fb->lo = (rate * per) / ISOC_FB_UFRAMES_PER_SEC;
	fb->lo -= fb->lo < tol ? fb->lo : tol;
	fb->hi = (rate * per + ISOC_FB_UFRAMES_PER_SEC - 1) / ISOC_FB_UFRAMES_PER_SEC + tol;
	return ok;
}

/*
 * isoc_fb_value - The frame count (COUNT) or 16.16 frames per microframe (Q16_16)
 * in a feedback packet.
 */
static inline uint32_t isoc_fb_value(const struct isoc_fb *fb, const uint8_t *pkt)
{
	if (fb->cfg.format == ISOC_FB_Q16_16)
		return (uint32_t)pkt[0] | ((uint32_t)pkt[1] << 8) | ((uint32_t)pkt[2] << 16) | ((uint32_t)pkt[3] << 24);
	return pkt[0];
}

/*
 * isoc_fb_packet - Take one feedback packet (@len bytes received) and fill the
 * ring entries of its interval. Returns true once, when the wrong-length packets
 * pass the limit and feedback stops being used.
 */
static inline bool isoc_fb_packet(struct isoc_fb *fb, const uint8_t *pkt, uint32_t len)
{
	uint32_t u = fb->uframes;
	uint32_t valid = len == fb->cfg.packet_size;
	uint32_t v = valid ? isoc_fb_value(fb, pkt) : 0;
	uint32_t whole = fb->cfg.format == ISOC_FB_Q16_16 ? v >> 16 : v;
	uint32_t ok = valid & (whole >= fb->lo) & (whole <= fb->hi) & !fb->broken;
	uint32_t k, n, lap_end;
	bool was_broken = fb->broken;

	for (k = 0; k < u; k++) {
		if (fb->cfg.format == ISOC_FB_Q16_16) {
			fb->q_acc += v * ok;
			n = fb->q_acc >> 16;
			fb->q_acc &= 0xFFFF;
		} else {
			n = ((v * (k + 1)) / u - (v * k) / u) * ok;   /* spread over the interval */
		}
		ISOC_FB_STORE(fb->ring[(fb->wr + k) & (ISOC_FB_RING - 1)], (uint8_t)n);
	}
	fb->wr += u;

	fb->errors += !valid;
	fb->lap_errors += !valid;
	lap_end = u && (fb->wr & (ISOC_FB_RING - 1)) < u;
	fb->errors *= !(lap_end && fb->lap_errors == 0);
	fb->lap_errors *= !lap_end;
	fb->broken |= fb->cfg.max_errors && fb->errors > fb->cfg.max_errors;

	return fb->broken && !was_broken;
}

/*
 * isoc_fb_out_frames - Frames for the next output packet, which covers @uframes
 * microframes: the device's counts from the ring, or the nominal rate where
 * there are none.
 */
static inline uint32_t isoc_fb_out_frames(struct isoc_fb *fb, uint32_t uframes)
{
	uint32_t frames = 0, k, n, nominal;

	for (k = 0; k < uframes; k++) {
		fb->nominal_acc += fb->rate;
		nominal = fb->nominal_acc / ISOC_FB_UFRAMES_PER_SEC;
		fb->nominal_acc -= nominal * ISOC_FB_UFRAMES_PER_SEC;

		n = ISOC_FB_LOAD(fb->ring[fb->rd & (ISOC_FB_RING - 1)]);
		fb->rd++;
		frames += n + (n == 0) * nominal;
	}
	return frames;
}

#ifdef __cplusplus
}
#endif

#endif /* OZZY_ISOC_FEEDBACK_H */
