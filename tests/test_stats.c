/* SPDX-License-Identifier: MIT */
/*
 * The shared memory's stats (engine/shared/ozzy_stats.h) on the DB4 at its defaults (96 kHz,
 * 80 frames x 4 queued each way, 833 us a transfer): completions on time are counted and give
 * the longest gap, nothing late; a gap past the queue (4 x 833 us plus 1 ms of jitter) is late
 * on the direction it held up, one just short of it isn't; an error, a short transfer and a
 * recovery count, each telling the platform (stats_changed); a stop's aborts are no gap after
 * the next start; a new attach starts from zero.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "fake_bus.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>

#define PCM_OUT  0x05
#define PCM_IN   0x86
#define PERIOD   833                /* us per 80-frame transfer at 96 kHz */

static struct ozzy_shared_memory shm;
static struct fake_bus fb;

/* The next PCM out completion @gap us after the last, and every PCM in / MIDI one queued before it */
static void step(struct ozzy_engine *e, uint64_t gap)
{
	struct fake_xfer x;

	while (fake_bus_pop(&fb, &x)) {
		if (x.pipe == PCM_OUT)
			fb.now += gap;
		fake_bus_complete(&fb, e, &x, NULL, NULL);
		if (x.pipe == PCM_OUT)
			return;
	}
}

/* Out and in in step: @n out completions @gap apart (an in one alongside each) */
static void run_for(struct ozzy_engine *e, unsigned int n, uint64_t gap)
{
	while (n--)
		step(e, gap);
}

static struct ozzy_engine *start(void)
{
	const struct ozzy_device *model = ozzy_device_lookup(0x0A4A, 0xFFDB);
	struct ozzy_engine *e = ozzy_engine_init(malloc(ozzy_engine_size(model)), model);

	memset(&shm, 0, sizeof(shm));
	fake_bus_init(&fb, &shm, true, NULL);
	fb.hz = 1000000;   /* 1 tick = 1 us */
	fb.now = 1000;
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_OUT, PCM_OUT, 3, 964, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_IN, PCM_IN, 3, 1024, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_MIDI_IN, 0x83, 2, 512, 0);
	ozzy_engine_attach(e, &fb.bus, &shm);
	CHECK(ozzy_engine_start(e), "start");
	return e;
}

int main(void)
{
	struct ozzy_engine *e = start();
	volatile struct ozzy_stats *s = &shm.stats;
	struct fake_xfer x;
	unsigned int told;

	CHECK(s->version == OZZY_STATS_VERSION && s->size == sizeof(struct ozzy_stats), "header %u/%u", s->version,
	      s->size);
	CHECK(shm.audio.sample_rate == 96000 && shm.info.transfer_frames[0] == 80 && shm.info.transfer_frames[1] == 80,
	      "runs at %u, %u/%u frames", shm.audio.sample_rate, shm.info.transfer_frames[0], shm.info.transfer_frames[1]);

	/* On time, with a little jitter */
	run_for(e, 200, PERIOD);
	run_for(e, 1, PERIOD + 300);
	run_for(e, 200, PERIOD);
	CHECK(s->dir[OZZY_PCM_OUT].transfers == 401 && s->dir[OZZY_PCM_IN].transfers >= 400,
	      "transfers %llu / %llu", (unsigned long long)s->dir[0].transfers, (unsigned long long)s->dir[1].transfers);
	CHECK(s->dir[OZZY_PCM_OUT].max_gap_us == PERIOD + 300, "out max gap %u us", s->dir[0].max_gap_us);
	CHECK(ozzy_stats_incidents(s) == 0 && fb.stats_changed == 0, "incidents %u, told %u", ozzy_stats_incidents(s),
	      fb.stats_changed);

	/* Just short of the queue's 3333 us plus 1 ms: not late; past it: late, both ways (the in
	 * completion waited as long) */
	run_for(e, 1, 4300);
	CHECK(s->dir[OZZY_PCM_OUT].late == 0, "a 4.3 ms gap was late");
	run_for(e, 1, 6000);
	run_for(e, 1, PERIOD);   /* the in completion queued behind it */
	CHECK(s->dir[OZZY_PCM_OUT].late == 1 && s->dir[OZZY_PCM_IN].late == 1, "a 6 ms gap: late %u / %u",
	      s->dir[0].late, s->dir[1].late);
	CHECK(s->dir[OZZY_PCM_OUT].max_gap_us == 6000 && s->dir[OZZY_PCM_IN].max_gap_us == 6000, "max gap %u / %u us",
	      s->dir[0].max_gap_us, s->dir[1].max_gap_us);
	CHECK(fb.stats_changed == 2, "told %u times of the two late ones", fb.stats_changed);

	/* An error and a short transfer, on whatever in transfer is queued */
	told = fb.stats_changed;
	while (fake_bus_pop(&fb, &x) && x.pipe != PCM_IN)
		fake_bus_complete(&fb, e, &x, NULL, NULL);
	ozzy_engine_complete(e, x.pipe, x.xfer, -5, 0);
	while (fake_bus_pop(&fb, &x) && x.pipe != PCM_IN)
		fake_bus_complete(&fb, e, &x, NULL, NULL);
	ozzy_engine_complete(e, x.pipe, x.xfer, 0, x.size - 64);
	CHECK(s->dir[OZZY_PCM_IN].errors == 1 && s->dir[OZZY_PCM_IN].short_transfers == 1,
	      "in errors %u, short %u", s->dir[1].errors, s->dir[1].short_transfers);
	CHECK(s->dir[OZZY_PCM_OUT].errors == 0 && s->dir[OZZY_PCM_OUT].short_transfers == 0, "out has incidents");
	CHECK(fb.stats_changed == told + 2, "told %u times of the error and the short one", fb.stats_changed - told);

	/* A restart: its aborts are no gap, and a long pause before the next start isn't late */
	ozzy_engine_suspend(e);
	fb.now += 10000000;
	CHECK(ozzy_engine_start(e), "restart");
	run_for(e, 50, PERIOD);
	CHECK(s->dir[OZZY_PCM_OUT].late == 1 && s->dir[OZZY_PCM_IN].late == 1, "the restart was late: %u / %u",
	      s->dir[0].late, s->dir[1].late);
	CHECK(s->dir[OZZY_PCM_OUT].errors == 0 && s->dir[OZZY_PCM_IN].errors == 1, "the aborts were errors");

	ozzy_engine_stats_recovery(e, true);
	ozzy_engine_stats_recovery(e, false);
	CHECK(s->recoveries == 2 && s->recovery_failures == 1, "recoveries %u, failed %u", s->recoveries,
	      s->recovery_failures);
	ozzy_engine_stop(e);
	free(e);

	/* A new attach: from zero */
	e = start();
	CHECK(ozzy_stats_incidents(s) == 0 && s->dir[0].transfers == 0 && s->dir[0].max_gap_us == 0, "not reset");
	ozzy_engine_stop(e);
	free(e);
	return CHECK_DONE("stats");
}
