/* SPDX-License-Identifier: MIT */
/*
 * The clock anchors of the Ploytec bulk / interrupt family (update_timestamp) on a device whose
 * clock is not the host's: a DB4 at 44.1 kHz in 160-frame transfers, anchors every 6400 frames
 * (the largest IO buffer at 2400), running 21 ppm slow, as one measured. Each out transfer
 * completes when the device has room for it, on the host's next 125 us microframe, and the
 * completion runs up to 30 us later. The anchors must lie on a straight line (the device's
 * clock) within a few us, bend by no more than a few us from one to the next, and give the
 * device's rate. Built from single completions they were 125 us steps every ~6 s.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "fake_bus.h"
#include "check.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PCM_OUT     0x05
#define HZ          24000000ull                 /* the clock: Apple silicon's mach ticks */
#define DEVICE_RATE (44100.0 * (1.0 - 21e-6))   /* the device's real rate on that clock */
#define MICROFRAME  (HZ / 8000)                 /* 125 us */
#define SECONDS     60
#define MAX_ANCHORS 1000

int main(void)
{
	const struct ozzy_device *model = ozzy_device_lookup(0x0A4A, 0xFFDB);
	static struct ozzy_shared_memory shm;
	static double frames[MAX_ANCHORS], ticks[MAX_ANCHORS];
	uint32_t jitter = 1, out_frames, n = 0, i;
	uint64_t played = 0, last_anchor = 0;
	double mx = 0, my = 0, sxx = 0, sxy = 0, slope, worst = 0, worst_bend = 0, power = 0, bend_power = 0;
	struct fake_bus fb;
	struct fake_xfer x;
	struct ozzy_engine *e;

	fake_bus_init(&fb, &shm, false, NULL);
	fb.hz = HZ;
	e = ozzy_engine_init(malloc(ozzy_engine_size(model)), model);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_OUT, PCM_OUT, 3, 964, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_IN, 0x86, 3, 1024, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_MIDI_IN, 0x83, 3, 64, 1);
	ozzy_engine_attach(e, &fb.bus, &shm);
	ozzy_engine_set_anchors(e, true);   /* as the macOS kext */
	CHECK(ozzy_engine_change_setting(e, OZZY_SETTING_PCM_OUT_FRAMES, 15) != OZZY_SETTING_INVALID &&
	      ozzy_engine_change_setting(e, OZZY_SETTING_IO_BUFFER_MAX, 3) != OZZY_SETTING_INVALID &&
	      ozzy_engine_change_rate(e, 44100) != OZZY_SETTING_INVALID, "settings refused");
	CHECK(ozzy_engine_start(e), "start");
	out_frames = shm.info.transfer_frames[OZZY_PCM_OUT];
	CHECK(out_frames == 160 && shm.audio.timestamp_frames == 6400 && shm.audio.sample_rate == 44100,
	      "running %u-frame transfers, anchors every %u, at %u Hz", out_frames, shm.audio.timestamp_frames,
	      shm.audio.sample_rate);

	while (fake_bus_pop(&fb, &x) && played < (uint64_t)SECONDS * 44100) {
		if (x.pipe == PCM_OUT) {
			/* The device has played this transfer's frames: the next microframe, then the
			 * completion's delay */
			double due = (double)(played += out_frames) / DEVICE_RATE * HZ;
			uint64_t on_bus = ((uint64_t)due / MICROFRAME + 1) * MICROFRAME;

			fb.now = on_bus + fake_rand(&jitter) % (HZ * 30 / 1000000);
		}
		fake_bus_complete(&fb, e, &x, NULL, NULL);
		if (shm.audio.timestamp.sample_time != last_anchor && n < MAX_ANCHORS) {
			last_anchor = shm.audio.timestamp.sample_time;
			frames[n] = (double)last_anchor;
			ticks[n++] = (double)shm.audio.timestamp.host_time;
		}
	}
	CHECK(n > SECONDS * 6, "%u anchors in %u s", n, SECONDS);

	/* The line through them all, and how far each is from it */
	for (i = 0; i < n; i++)
		mx += frames[i] / n, my += ticks[i] / n;
	for (i = 0; i < n; i++)
		sxx += (frames[i] - mx) * (frames[i] - mx), sxy += (frames[i] - mx) * (ticks[i] - my);
	slope = sxy / sxx;
	for (i = 0; i < n; i++) {
		double off = fabs(ticks[i] - (my + slope * (frames[i] - mx))) * 1e6 / HZ;

		worst = off > worst ? off : worst;
		power += off * off;
		if (i > 0 && i + 1 < n) {
			double bend = fabs(ticks[i + 1] - 2 * ticks[i] + ticks[i - 1]) * 1e6 / HZ;

			worst_bend = bend > worst_bend ? bend : worst_bend;
			bend_power += bend * bend;
		}
	}
	/* What is left is the completions' delay, averaged (2 us rms off the line, bends of 4 us rms
	 * and 12 us at worst); anchors from single completions were 37 us rms and bent 137 us, as a
	 * real DB4's did */
	CHECK(fabs(HZ / slope / DEVICE_RATE - 1) < 0.2e-6, "rate %.3f Hz, the device runs at %.3f", HZ / slope,
	      DEVICE_RATE);
	CHECK(sqrt(power / n) < 4 && worst < 10, "anchors off the device's line by %.1f us rms, %.1f at worst",
	      sqrt(power / n), worst);
	CHECK(sqrt(bend_power / (n - 2)) < 8 && worst_bend < 25, "the clock bends by %.1f us rms, %.1f at worst",
	      sqrt(bend_power / (n - 2)), worst_bend);

	ozzy_engine_stop(e);
	free(e);
	return CHECK_DONE("clock");
}
