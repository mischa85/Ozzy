/* SPDX-License-Identifier: MIT */
/*
 * The stall watchdog, recovery, sleep and wake (ozzy_engine_watch, _recover, _sleep, _wake) on
 * the DB4 at its defaults (96 kHz, 833 us a transfer): completions on time and a gap short of
 * OZZY_WATCH_DEAD_MS are no stall; a PCM pipe silent for longer is reported once, then not again
 * for OZZY_WATCH_RECOVER_S; a recovery resets the device through the bus, starts again, counts
 * in the stats and gives the start a fresh limit; a failed one leaves the engine stopped. Sleep
 * suspends (nothing reported however long it lasts), wake starts again; a stop in between
 * (the device left during sleep) isn't started by the wake.
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
#define MS       1000               /* 1 tick = 1 us */

static struct ozzy_shared_memory shm;
static struct fake_bus fb;

/* @n PCM out completions @gap us apart, with whatever else was queued before each */
static void run_for(struct ozzy_engine *e, unsigned int n, uint64_t gap)
{
	struct fake_xfer x;

	while (n--) {
		while (fake_bus_pop(&fb, &x)) {
			if (x.pipe == PCM_OUT)
				fb.now += gap;
			fake_bus_complete(&fb, e, &x, NULL, NULL);
			if (x.pipe == PCM_OUT)
				break;
		}
	}
}

int main(void)
{
	const struct ozzy_device *model = ozzy_device_lookup(0x0A4A, 0xFFDB);
	struct ozzy_engine *e = ozzy_engine_init(malloc(ozzy_engine_size(model)), model);
	uint32_t starts;

	fake_bus_init(&fb, &shm, true, NULL);
	fb.hz = 1000000;
	fb.now = 1000;
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_OUT, PCM_OUT, 3, 964, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_IN, PCM_IN, 3, 1024, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_MIDI_IN, 0x83, 2, 512, 0);
	ozzy_engine_attach(e, &fb.bus, &shm);

	/* Not started: nothing to watch */
	fb.now += 2000 * MS;
	CHECK(!ozzy_engine_streaming(e) && ozzy_engine_watch(e) == 0, "watched before the start");

	CHECK(ozzy_engine_start(e) && ozzy_engine_streaming(e), "start");
	/* The start took long (the handshake): its limit counts from its end */
	CHECK(ozzy_engine_watch(e) == 0, "the start itself was a stall");

	/* On time, then a gap that comes back on its own */
	run_for(e, 100, PERIOD);
	CHECK(ozzy_engine_watch(e) == 0, "stall while on time");
	fb.now += 400 * MS;
	CHECK(ozzy_engine_watch(e) == 0, "a 400 ms gap was a stall");
	run_for(e, 10, PERIOD);
	CHECK(ozzy_engine_watch(e) == 0, "stall after it came back");

	/* Silent past the limit: reported once, not again within OZZY_WATCH_RECOVER_S */
	fb.now += (OZZY_WATCH_DEAD_MS + 10) * MS;
	CHECK(ozzy_engine_watch(e) == PCM_OUT, "no stall reported after %u ms", OZZY_WATCH_DEAD_MS + 10);
	CHECK(ozzy_engine_watch(e) == 0, "reported twice");

	/* The recovery: reset, a new start, counted; a fresh limit from its start */
	starts = e->starts;
	CHECK(ozzy_engine_recover(e) && ozzy_engine_streaming(e), "recovery didn't stream");
	CHECK(fb.resets == 1 && e->starts == starts + 1, "resets %u, starts %u", fb.resets, e->starts - starts);
	CHECK(shm.stats.recoveries == 1 && shm.stats.recovery_failures == 0, "recoveries %u/%u", shm.stats.recoveries,
	      shm.stats.recovery_failures);
	CHECK(ozzy_engine_watch(e) == 0, "stall right after the recovery");

	/* Dead again within OZZY_WATCH_RECOVER_S: left alone; after it: reported */
	fb.now += 2000 * MS;
	CHECK(ozzy_engine_watch(e) == 0, "recovered again within %u s", OZZY_WATCH_RECOVER_S);
	fb.now += OZZY_WATCH_RECOVER_S * 1000 * MS;
	CHECK(ozzy_engine_watch(e) != 0, "not reported after %u s", OZZY_WATCH_RECOVER_S);
	CHECK(ozzy_engine_recover(e), "second recovery");
	run_for(e, 20, PERIOD);

	/* Sleep: suspended, no stall however long; wake: a new start */
	starts = e->starts;
	ozzy_engine_sleep(e);
	CHECK(!ozzy_engine_streaming(e) && e->sleeping, "not suspended for sleep");
	fb.now += 60000 * MS;
	CHECK(ozzy_engine_watch(e) == 0, "stall reported during sleep");
	CHECK(ozzy_engine_wake(e) && ozzy_engine_streaming(e) && e->starts == starts + 1, "wake didn't start");
	CHECK(ozzy_engine_watch(e) == 0, "stall right after wake");
	CHECK(ozzy_engine_wake(e) && e->starts == starts + 1, "a second wake started again");

	/* The device leaves during sleep: the wake doesn't start it */
	ozzy_engine_sleep(e);
	ozzy_engine_stop(e);
	starts = e->starts;
	CHECK(!ozzy_engine_wake(e) && e->starts == starts && !ozzy_engine_streaming(e), "woke a stopped engine");

	/* Sleep while not streaming: nothing to wake */
	ozzy_engine_sleep(e);
	CHECK(!e->sleeping && !ozzy_engine_wake(e), "slept a stopped engine");

	free(e);
	return CHECK_DONE("watch");
}
