/* SPDX-License-Identifier: MIT */
/*
 * A fake USB bus for host tests of engine/ (fake_bus.h).
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "fake_bus.h"
#include <string.h>

static long shm_offset(struct fake_bus *fb, const void *p)
{
	return (long)((const uint8_t *)p - (const uint8_t *)fb->shm);
}

static bool fb_submit(void *ctx, uint8_t pipe, void *buf, uint32_t size, void *xfer)
{
	struct fake_bus *fb = ctx;
	struct fake_xfer x = { pipe, buf, size, xfer, false, NULL, 0 };

	if (fb->trace)
		fprintf(fb->trace, "submit %02x off=%ld size=%u xfer=%lu\n", pipe, shm_offset(fb, buf), size,
			(unsigned long)(uintptr_t)xfer);
	fake_bus_push(fb, &x);
	return true;
}

static bool fb_submit_isoc(void *ctx, uint8_t pipe, void *buf, uint32_t buf_size, struct ozzy_isoc_packet *packets,
			   uint32_t count, void *xfer)
{
	struct fake_bus *fb = ctx;
	struct fake_xfer x = { pipe, buf, buf_size, xfer, true, packets, count };
	uint32_t i;

	if (fb->trace) {
		fprintf(fb->trace, "isoc %02x off=%ld size=%u xfer=%lu:", pipe, shm_offset(fb, buf), buf_size,
			(unsigned long)(uintptr_t)xfer);
		for (i = 0; i < count; i++)
			fprintf(fb->trace, " %u/%u", packets[i].offset, packets[i].length);
		fprintf(fb->trace, "\n");
	}
	fake_bus_push(fb, &x);
	return true;
}

static bool fb_control(void *ctx, uint8_t type, uint8_t req, uint16_t val, uint16_t idx, void *data, uint16_t len)
{
	struct fake_bus *fb = ctx;

	if (fb->trace)
		fprintf(fb->trace, "control %02x %02x %04x %04x %u\n", type, req, val, idx, len);
	if (type == 0x22 && req == 0x01 && len >= 3) {
		memcpy(fb->rate, data, 3);
	} else if ((type & 0x80) && len) {
		memset(data, 0, len);
		if (type == 0xA2 && req == 0x81 && len >= 3)
			memcpy(data, fb->rate, 3);
	}
	return true;
}

static int fb_clear_halt(void *ctx, uint8_t endpoint)
{
	struct fake_bus *fb = ctx;

	if (fb->trace)
		fprintf(fb->trace, "clear_halt %02x\n", endpoint);
	return 0;
}

static bool fb_start_timer(void *ctx, uint32_t interval_us)
{
	struct fake_bus *fb = ctx;

	if (fb->trace)
		fprintf(fb->trace, "timer %u\n", interval_us);
	fb->timer_on = true;
	return true;
}

static void fb_stop_timer(void *ctx)
{
	((struct fake_bus *)ctx)->timer_on = false;
}

static void fb_pcm_progress(void *ctx, enum ozzy_pcm_dir dir, uint64_t frames)
{
	struct fake_bus *fb = ctx;

	if (fb->trace)
		fprintf(fb->trace, "progress %d %llu\n", (int)dir, (unsigned long long)frames);
	fb->progress[dir] = frames;
}

static void fb_sleep(void *ctx, unsigned int ms)
{
}

static uint64_t fb_time(void *ctx)
{
	return ++((struct fake_bus *)ctx)->ticks;
}

/* Everything queued completes as aborted (to the engine of the last completion, if any) */
static void fb_abort(void *ctx)
{
	struct fake_bus *fb = ctx;
	struct fake_xfer x;
	unsigned int n = 0;

	while (fake_bus_pop(fb, &x)) {
		n++;
		if (!fb->engine)
			continue;
		if (x.isoc)
			ozzy_engine_isoc_complete(fb->engine, x.pipe, x.xfer, OZZY_STATUS_ABORTED, x.packets, x.count);
		else
			ozzy_engine_complete(fb->engine, x.pipe, x.xfer, OZZY_STATUS_ABORTED, 0);
	}
	if (fb->trace)
		fprintf(fb->trace, "abort %u\n", n);
}

static const struct ozzy_bus_ops fake_ops = {
	.submit = fb_submit,
	.submit_isoc = fb_submit_isoc,
	.control = fb_control,
	.clear_halt = fb_clear_halt,
	.start_timer = fb_start_timer,
	.stop_timer = fb_stop_timer,
	.pcm_progress = fb_pcm_progress,
	.sleep_ms = fb_sleep,
	.time = fb_time,
	.abort = fb_abort,
};

static const struct ozzy_bus_ops fake_ops_no_rings = {
	.submit = fb_submit,
	.submit_isoc = fb_submit_isoc,
	.control = fb_control,
	.clear_halt = fb_clear_halt,
	.start_timer = fb_start_timer,
	.stop_timer = fb_stop_timer,
	.sleep_ms = fb_sleep,
	.time = fb_time,
	.abort = fb_abort,
};

void fake_bus_init(struct fake_bus *fb, struct ozzy_shared_memory *shm, bool rings, FILE *trace)
{
	memset(fb, 0, sizeof(*fb));
	fb->bus.ops = rings ? &fake_ops : &fake_ops_no_rings;
	fb->bus.ctx = fb;
	fb->shm = shm;
	fb->trace = trace;
	fb->rings = rings;
}

void fake_bus_push(struct fake_bus *fb, const struct fake_xfer *x)
{
	fb->queue[fb->tail++ % 512] = *x;
}

bool fake_bus_pop(struct fake_bus *fb, struct fake_xfer *x)
{
	if (fb->head == fb->tail)
		return false;
	*x = fb->queue[fb->head++ % 512];
	return true;
}

void fake_bus_complete(struct fake_bus *fb, struct ozzy_engine *e, struct fake_xfer *x,
		       void (*fill)(struct fake_xfer *x, void *arg), void *arg)
{
	uint32_t i;

	fb->engine = e;
	if ((x->pipe & 0x80) && fill)
		fill(x, arg);
	if (x->isoc) {
		for (i = 0; i < x->count; i++) {
			x->packets[i].actual = x->packets[i].length;
			x->packets[i].status = 0;
			x->packets[i].time = fb->ticks + 1000;
		}
		ozzy_engine_isoc_complete(e, x->pipe, x->xfer, 0, x->packets, x->count);
	} else {
		ozzy_engine_complete(e, x->pipe, x->xfer, 0, x->size);
	}
}
