/* SPDX-License-Identifier: MIT */
/*
 * The isochronous ring path (Tascam US-144MKII: 4 in, 4 out, both routings): every frame
 * that goes out on the wire must be the next routed ring frame, in order; captured frames
 * must be the routed decoded ones.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "fake_bus.h"
#include "check.h"
#include "common/devices/ploytec/ploytec_protocol.h"
#include "common/devices/ploytec/ploytec_codec.h"
#include <stdlib.h>
#include <string.h>

static uint32_t seed = 7;
static uint8_t sent[200000 * 12];
static uint64_t sent_frames;

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

/* What an OUT transfer puts on the wire, in order (packets may wrap the ring) */
static void record_out(const struct fake_xfer *x)
{
	uint32_t k, j;

	for (k = 0; k < x->count; k++) {
		for (j = 0; j < x->packets[k].length; j++)
			sent[sent_frames * 12 + j] = x->buf[(x->packets[k].offset + j) % x->size];
		sent_frames += x->packets[k].length / 12;
	}
}

int main(void)
{
	const struct ozzy_device *model = ozzy_device_lookup(0x0644, 0x8020);
	static struct ozzy_shared_memory shm;
	static uint8_t out_ring[1000 * 12], in_ring[1000 * 12];
	struct ozzy_pcm_ring ro = { out_ring, 1000, 4, 0 }, ri = { in_ring, 1000, 4, 0 };
	struct fake_bus fb;
	struct fake_xfer x;
	struct ozzy_engine *e;
	unsigned int i, ins = 0, wrong = 0, wrong_in = 0;

	CHECK(model && model->family == OZZY_FAMILY_PLOYTEC_ISOC, "US-144MKII in the device list");
	for (i = 0; i < sizeof(out_ring); i++)
		out_ring[i] = (uint8_t)(i * 7 + 3);
	fake_bus_init(&fb, &shm, true, NULL);
	e = ozzy_engine_init(malloc(ozzy_engine_size(model)), model);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_OUT, 0x02, 1, 1024, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_IN, 0x86, 2, 512, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_FEEDBACK, 0x81, 1, 3, 4);
	ozzy_engine_attach(e, &fb.bus, &shm);
	ozzy_engine_set_setting(e, OZZY_SETTING_OUT_ROUTE_1, 1);   /* outputs 1-2 from playback 3-4 */
	ozzy_engine_set_setting(e, OZZY_SETTING_IN_ROUTE_1, 1);    /* capture 1-2 from inputs 3-4 */
	ozzy_engine_set_pcm_ring(e, OZZY_PCM_OUT, &ro);
	ozzy_engine_set_pcm_ring(e, OZZY_PCM_IN, &ri);
	CHECK(ozzy_engine_start(e), "start");
	/* The family's own defaults: 80-frame in transfers, 2 in flight, input safety what they queue */
	CHECK(ozzy_engine_has_setting(e, OZZY_SETTING_PCM_IN_QUEUE) && !ozzy_engine_has_setting(e, OZZY_SETTING_PCM_OUT_QUEUE) &&
	      !ozzy_engine_has_setting(e, OZZY_SETTING_PCM_OUT_FRAMES), "isoc: the wrong transfer settings");
	CHECK(ozzy_pcm_queue(e->settings.value[OZZY_SETTING_PCM_IN_QUEUE]) == 2 &&
	      shm.info.pipes[OZZY_PIPE_PCM_IN].in_flight == 2, "isoc: in queue %u by default, %u in flight",
	      ozzy_pcm_queue(e->settings.value[OZZY_SETTING_PCM_IN_QUEUE]), shm.info.pipes[OZZY_PIPE_PCM_IN].in_flight);
	CHECK(shm.audio.input_safety_offset_frames == 2 * 80, "isoc: input safety %u", shm.audio.input_safety_offset_frames);

	for (i = 0; i < 300 && fake_bus_pop(&fb, &x); i++) {
		if (x.isoc && !(x.pipe & 0x80))
			record_out(&x);
		ins += x.pipe == 0x86;
		fake_bus_complete(&fb, e, &x, fill, NULL);
	}
	for (i = 0; i < sent_frames; i++) {
		const uint8_t *src = out_ring + (i % 1000) * 12;

		wrong += memcmp(sent + i * 12, src + 6, 6) || memcmp(sent + i * 12 + 6, src + 6, 6);
	}
	/* The last IN transfer's frames (80, end to end in the ring) */
	for (i = 0; i < 80 && ins; i++) {
		unsigned int j = ins - 1;
		const uint8_t *pkt = (const uint8_t *)shm.audio.input_buffer +
				     (j % (shm.audio.input_ring_frames / 80)) * 80 * PLOYTEC_IN_FRAME_SIZE;
		uint32_t r = (j * 80 + i) % 1000;
		uint8_t dec[12];

		ploytec_decode_frame_4ch(dec, pkt + i * PLOYTEC_IN_FRAME_SIZE);
		wrong_in += memcmp(in_ring + r * 12, dec + 6, 6) || memcmp(in_ring + r * 12 + 6, dec + 6, 6);
	}
	CHECK(sent_frames > 10000, "%llu frames sent", (unsigned long long)sent_frames);
	CHECK(!wrong, "%u of %llu frames sent aren't the routed ring frames", wrong, (unsigned long long)sent_frames);
	CHECK(fb.progress[OZZY_PCM_OUT] >= sent_frames, "out progress %llu", (unsigned long long)fb.progress[OZZY_PCM_OUT]);
	CHECK(ins > 10 && !wrong_in, "%u IN packets, %u captured frames wrong", ins, wrong_in);
	CHECK(fb.progress[OZZY_PCM_IN] == (uint64_t)ins * 80, "in progress %llu", (unsigned long long)fb.progress[OZZY_PCM_IN]);
	free(e);
	return CHECK_DONE("isochronous rings");
}
