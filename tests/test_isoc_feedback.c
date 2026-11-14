/* SPDX-License-Identifier: MIT */
/*
 * Isochronous output pacing (common/isoc_feedback.h): when feedback is used and
 * when it is given up.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "check.h"
#include "common/isoc_feedback.h"

/* The Ploytec format: 3-byte packets, byte 0 the frames played; a packet every 8 microframes */
static const struct isoc_fb_config ploytec = { ISOC_FB_COUNT, 3, 40 };

/* Feed @count packets of @len bytes, each saying @frames; true if feedback was given up */
static bool feed(struct isoc_fb *fb, uint32_t count, uint32_t len, uint8_t frames)
{
	const uint8_t pkt[3] = { frames, frames, frames };
	bool gave_up = false;
	uint32_t i;

	for (i = 0; i < count; i++)
		gave_up |= isoc_fb_packet(fb, pkt, len);
	return gave_up;
}

int main(void)
{
	struct isoc_fb fb;

	/* A device that starts with empty packets (PHASE X64, #97) keeps its feedback */
	CHECK(isoc_fb_init(&fb, 48000, &ploytec, 4), "48 kHz, bInterval 4");
	CHECK(!feed(&fb, 200, 0, 0), "empty packets before the first valid one don't count");
	CHECK(!fb.broken, "feedback still used after 200 empty packets at the start");
	CHECK(!feed(&fb, 32, 3, 49), "valid packets");
	CHECK(fb.ring[(fb.wr - 1) & (ISOC_FB_RING - 1)] != 0, "valid feedback lands in the ring");

	/* Once feedback came in, wrong lengths count: past 40 without a clean lap it is given up */
	CHECK(feed(&fb, 41, 0, 0), "41 empty packets after valid ones give feedback up");
	CHECK(fb.broken, "feedback ignored");

	/* A restart (rate change) starts over */
	CHECK(isoc_fb_init(&fb, 44100, &ploytec, 4), "44.1 kHz");
	CHECK(!fb.broken && !fb.started, "a new stream waits for its first valid packet again");

	/* A few bad packets with clean laps in between are fine */
	CHECK(!feed(&fb, 4, 3, 44), "valid packets");
	CHECK(!feed(&fb, 10, 2, 0), "10 short packets");
	CHECK(!feed(&fb, 32, 3, 44), "valid packets through a whole clean lap (16 packets of 8 microframes)");
	CHECK(!feed(&fb, 35, 2, 0), "35 more short packets after the clean lap");
	CHECK(!fb.broken, "the clean lap reset the count");

	return CHECK_DONE("isochronous feedback");
}
