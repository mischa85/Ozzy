/* SPDX-License-Identifier: MIT */
/*
 * The engine on transfer memory of the platform's own (ozzy_engine_alloc_memory), as Linux has
 * it: separate buffers, 4 PCM slots per direction, a buffer per isochronous OUT transfer.
 * Every transfer must use that memory (never the shared memory's HAL rings), the frames on
 * the wire must be the ring's frames in order, no isochronous packet may wrap or leave a gap
 * (Windows sends a transfer as one URB), and everything allocated must be freed.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "fake_bus.h"
#include "check.h"
#include "common/devices/ploytec/ploytec_protocol.h"
#include "common/devices/ploytec/ploytec_codec.h"
#include <stdlib.h>
#include <string.h>

struct platform_memory {
	uint8_t *blocks[32];
	size_t sizes[32];
	unsigned int count;
	unsigned int freed;
};

static void *pm_alloc(void *ctx, size_t size)
{
	struct platform_memory *pm = ctx;
	uint8_t *b = malloc(size);

	memset(b, 0xA5, size);   /* the engine zeroes it */
	pm->blocks[pm->count] = b;
	pm->sizes[pm->count++] = size;
	return b;
}

static void pm_free(void *ctx, void *p)
{
	struct platform_memory *pm = ctx;
	unsigned int i;

	for (i = 0; i < pm->count; i++)
		if (pm->blocks[i] == p) {
			free(p);
			pm->blocks[i] = NULL;
			pm->freed++;
		}
}

/* Whether [p, p + size) lies inside one of the platform's blocks */
static bool in_platform_memory(const struct platform_memory *pm, const uint8_t *p, uint32_t size)
{
	unsigned int i;

	for (i = 0; i < pm->count; i++)
		if (p >= pm->blocks[i] && p + size <= pm->blocks[i] + pm->sizes[i])
			return true;
	return false;
}

/* Free the engine's memory: every block must come back */
static void platform_memory_done(struct platform_memory *pm, struct ozzy_engine *e, const char *name)
{
	ozzy_engine_free_memory(e);
	CHECK(pm->count > 0 && pm->freed == pm->count, "%s: %u of %u blocks freed", name, pm->freed, pm->count);
}

static bool all_zero(const volatile uint8_t *p, size_t n)
{
	while (n--)
		if (*p++)
			return false;
	return true;
}

static uint32_t seed = 3;

static void fill(struct fake_xfer *x, void *arg)
{
	uint32_t i, k;

	if (x->isoc) {
		for (k = 0; k < x->count; k++) {
			uint8_t *b = x->buf + x->packets[k].offset;

			b[0] = 0x00;
			b[1] = 0x00;
			b[2] = 0x0C;
		}
		return;
	}
	for (i = 0; i < x->size; i++)
		x->buf[i] = (uint8_t)fake_rand(&seed);
}

/* Xone:DB4 with 4 PCM slots: the frames of OUT transfer n >= 4 are ring frames (n - 4) * 80.. */
static void xone(void)
{
	const struct ozzy_device *model = ozzy_device_lookup(0x0A4A, 0xFFDB);
	static struct ozzy_shared_memory shm;
	static uint8_t ring[800 * 24];
	struct ozzy_pcm_ring ro = { ring, 800, 8, 0 };
	struct platform_memory pm = { { 0 } };
	struct ozzy_allocator a = { pm_alloc, pm_free, &pm };
	struct fake_bus fb;
	struct fake_xfer x;
	struct ozzy_engine *e;
	unsigned int i, f, outside = 0, wrong = 0, outs = 0;

	memset(&shm, 0, sizeof(shm));
	for (i = 0; i < sizeof(ring); i++)
		ring[i] = (uint8_t)(i * 11 + 1);
	fake_bus_init(&fb, &shm, true, NULL);
	e = ozzy_engine_init(malloc(ozzy_engine_size(model)), model);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_OUT, 0x05, 3, 964, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_IN, 0x86, 3, 1024, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_MIDI_IN, 0x83, 3, 64, 1);
	ozzy_engine_attach(e, &fb.bus, &shm);
	CHECK(ozzy_engine_alloc_memory(e, &a), "alloc");
	ozzy_engine_set_pcm_ring(e, OZZY_PCM_OUT, &ro);
	CHECK(ozzy_engine_start(e), "Xone start");

	for (i = 0; i < 400 && fake_bus_pop(&fb, &x); i++) {
		outside += !in_platform_memory(&pm, x.buf, x.size);
		if (x.pipe == 0x05) {
			uint32_t n = (uint32_t)(uintptr_t)x.xfer;

			for (f = 0; n >= 4 && f < 80; f++) {   /* the default transfer: 80 frames */
				uint8_t enc[48];

				ploytec_encode_frame_8ch(enc, ring + (((n - 4) * 80 + f) % 800) * 24);
				wrong += memcmp(enc, x.buf + ploytec_out_frame_offset(PLOYTEC_OUT_LAYOUT_INT, f), 48) != 0;
			}
			outs += n >= 4;
		}
		fake_bus_complete(&fb, e, &x, fill, NULL);
	}
	CHECK(!outside, "Xone: %u transfers outside the platform's memory", outside);
	CHECK(outs > 50 && !wrong, "Xone: %u ring-encoded OUT transfers, %u wrong frames", outs, wrong);
	CHECK(all_zero(shm.audio.output_buffer, sizeof(shm.audio.output_buffer)) &&
	      all_zero(shm.audio.input_buffer, sizeof(shm.audio.input_buffer)), "Xone: the HAL rings were touched");
	ozzy_engine_stop(e);
	platform_memory_done(&pm, e, "Xone");
	free(e);
}

/* Akai EIE Pro with a buffer per OUT transfer: the frames on the wire are the ring's, in order */
static void isoc(void)
{
	const struct ozzy_device *model = ozzy_device_lookup(0x09E8, 0x0010);
	static struct ozzy_shared_memory shm;
	static uint8_t ring[1000 * 12];
	struct ozzy_pcm_ring ro = { ring, 1000, 4, 0 };
	struct platform_memory pm = { { 0 } };
	struct ozzy_allocator a = { pm_alloc, pm_free, &pm };
	struct fake_bus fb;
	struct fake_xfer x;
	struct ozzy_engine *e;
	uint64_t frame = 0;
	unsigned int i, k, outside = 0, wrapped = 0, wrong = 0, gaps = 0;

	memset(&shm, 0, sizeof(shm));
	for (i = 0; i < sizeof(ring); i++)
		ring[i] = (uint8_t)(i * 5 + 9);
	fake_bus_init(&fb, &shm, true, NULL);
	e = ozzy_engine_init(malloc(ozzy_engine_size(model)), model);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_OUT, 0x02, 1, 1024, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_IN, 0x86, 2, 512, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_MIDI_IN, 0x83, 2, 64, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_MIDI_OUT, 0x03, 2, 64, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_FEEDBACK, 0x81, 1, 3, 4);
	ozzy_engine_attach(e, &fb.bus, &shm);
	CHECK(ozzy_engine_alloc_memory(e, &a), "alloc");
	ozzy_engine_set_pcm_ring(e, OZZY_PCM_OUT, &ro);
	CHECK(ozzy_engine_start(e), "isoc start");

	for (i = 0; i < 400 && fake_bus_pop(&fb, &x); i++) {
		outside += !in_platform_memory(&pm, x.buf, x.size);
		/* One Windows URB per transfer: packets back to back from the first one's offset */
		for (k = 1; x.isoc && k < x.count; k++)
			gaps += x.packets[k].offset != x.packets[k - 1].offset + x.packets[k - 1].length;
		if (x.isoc && x.pipe == 0x02) {
			for (k = 0; k < x.count; k++) {
				uint32_t j;

				wrapped += x.packets[k].offset + x.packets[k].length > x.size;
				for (j = 0; j < x.packets[k].length / 12; j++, frame++)
					wrong += memcmp(x.buf + x.packets[k].offset + j * 12, ring + (frame % 1000) * 12, 12) != 0;
			}
		}
		if (fb.timer_on && i % 3 == 0)
			ozzy_engine_timer(e);
		fake_bus_complete(&fb, e, &x, fill, NULL);
	}
	CHECK(!outside, "isoc: %u transfers outside the platform's memory", outside);
	CHECK(!wrapped, "isoc: %u packets wrap their transfer buffer", wrapped);
	CHECK(!gaps, "isoc: %u packets not right after the one before", gaps);
	CHECK(frame > 10000 && !wrong, "isoc: %llu frames on the wire, %u wrong", (unsigned long long)frame, wrong);
	CHECK(all_zero(shm.audio.output_buffer, sizeof(shm.audio.output_buffer)), "isoc: the HAL ring was touched");
	CHECK(e->mem.isoc_out_size == 32 * 1024, "isoc: OUT transfer buffers of %u bytes", e->mem.isoc_out_size);
	ozzy_engine_stop(e);
	platform_memory_done(&pm, e, "isoc");
	free(e);
}

int main(void)
{
	xone();
	isoc();
	return CHECK_DONE("platform memory");
}
