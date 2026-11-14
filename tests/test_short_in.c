/* SPDX-License-Identifier: MIT */
/*
 * A short PCM IN transfer that came through (the DB4 starts its IN stream with one 512-byte
 * packet, 8 frames, at some starts) is the frames it carried: on a ring platform those 8 frames
 * go into the ring and the input moves on by 8, the next transfer right after them; a failed
 * transfer still leaves its full 80 frames of silence. Without a ring (macOS), the part of the
 * transfer's place no frame reached is zeroed, so nothing an earlier start left there is read.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "fake_bus.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>

#define PCM_IN    0x86
#define FRAMES    80
#define IN_FRAME  64                 /* bytes of an encoded input frame */
#define CH        8
#define RING      1000

static struct ozzy_shared_memory shm;
static struct fake_bus fb;
static uint8_t ring_mem[RING * CH * 3];

/* A transfer of @bytes, every byte set (full-scale frames) */
static void fill_ones(struct fake_xfer *x, void *arg)
{
	uint32_t bytes = *(uint32_t *)arg;

	memset(x->buf, 0xFF, bytes);
	x->size = bytes;
}

static struct ozzy_engine *start(bool rings)
{
	const struct ozzy_device *model = ozzy_device_lookup(0x0A4A, 0xFFDB);
	struct ozzy_engine *e = ozzy_engine_init(malloc(ozzy_engine_size(model)), model);

	memset(&shm, 0, sizeof(shm));
	fake_bus_init(&fb, &shm, rings, NULL);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_OUT, 0x05, 3, 964, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_IN, PCM_IN, 3, 1024, 1);
	ozzy_engine_attach(e, &fb.bus, &shm);
	CHECK(ozzy_engine_start(e), "start");
	return e;
}

/* The next PCM IN transfer, everything queued before it completed in full */
static bool next_in(struct ozzy_engine *e, struct fake_xfer *x)
{
	while (fake_bus_pop(&fb, x)) {
		if (x->pipe == PCM_IN)
			return true;
		fake_bus_complete(&fb, e, x, NULL, NULL);
	}
	return false;
}

/* Frames @from..@to of the ring all set (full scale) or all zero */
static bool ring_frames(uint32_t from, uint32_t to, bool set)
{
	for (uint32_t i = from * CH * 3; i < to * CH * 3; i++)
		if ((ring_mem[i] != 0) != set)
			return false;
	return true;
}

int main(void)
{
	struct ozzy_pcm_ring ring = { ring_mem, RING, CH, 0 };
	struct ozzy_engine *e;
	struct fake_xfer x;
	uint32_t bytes;

	/* --- A ring platform (Linux, Windows) --- */
	e = start(true);
	ozzy_engine_set_pcm_ring(e, OZZY_PCM_IN, &ring);

	/* The first transfer of the start: one 512-byte packet, 8 frames */
	CHECK(next_in(e, &x) && x.size == FRAMES * IN_FRAME, "no PCM in transfer of %u bytes", x.size);
	bytes = 8 * IN_FRAME;
	fake_bus_complete(&fb, e, &x, fill_ones, &bytes);
	CHECK(e->pcm_pos[OZZY_PCM_IN] == 8, "the short transfer moved the input by %llu frames",
	      (unsigned long long)e->pcm_pos[OZZY_PCM_IN]);
	CHECK(ring_frames(0, 8, true) && ring_frames(8, 100, false), "the short transfer's frames aren't 0-7");
	CHECK(shm.stats.dir[OZZY_PCM_IN].short_transfers == 1, "not counted as short");

	/* The next one goes on right after them */
	CHECK(next_in(e, &x), "no second PCM in transfer");
	bytes = FRAMES * IN_FRAME;
	fake_bus_complete(&fb, e, &x, fill_ones, &bytes);
	CHECK(e->pcm_pos[OZZY_PCM_IN] == 8 + FRAMES && ring_frames(0, 8 + FRAMES, true),
	      "a full transfer after it: position %llu", (unsigned long long)e->pcm_pos[OZZY_PCM_IN]);

	/* A failed one: the transfer's time as silence */
	CHECK(next_in(e, &x), "no third PCM in transfer");
	ozzy_engine_complete(e, x.pipe, x.xfer, -5, 0);
	CHECK(e->pcm_pos[OZZY_PCM_IN] == 8 + 2 * FRAMES && ring_frames(8 + FRAMES, 8 + 2 * FRAMES, false),
	      "a failed transfer: position %llu", (unsigned long long)e->pcm_pos[OZZY_PCM_IN]);
	ozzy_engine_set_pcm_ring(e, OZZY_PCM_IN, NULL);
	ozzy_engine_stop(e);
	free(e);

	/* --- No ring (macOS): the rest of the place zeroed, the frames that came kept --- */
	e = start(false);
	CHECK(next_in(e, &x), "no PCM in transfer without a ring");
	memset(x.buf, 0xAA, FRAMES * IN_FRAME);   /* what an earlier start left */
	bytes = 8 * IN_FRAME;
	fake_bus_complete(&fb, e, &x, fill_ones, &bytes);
	{
		bool kept = true, zeroed = true;

		for (uint32_t i = 0; i < 8 * IN_FRAME; i++)
			kept &= x.buf[i] == 0xFF;
		for (uint32_t i = 8 * IN_FRAME; i < FRAMES * IN_FRAME; i++)
			zeroed &= x.buf[i] == 0;
		CHECK(kept && zeroed, "without a ring: frames kept %d, the rest zeroed %d", kept, zeroed);
	}
	ozzy_engine_stop(e);
	free(e);
	return CHECK_DONE("short in");
}
