/* SPDX-License-Identifier: MIT */
/*
 * The ring path on a Xone with both routings (a DB4 given the routing quirks: no Xone has
 * them yet), and the rate request: the frames sent must be the routed ring frames, the ring
 * must get the routed decoded frames.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "fake_bus.h"
#include "check.h"
#include "common/devices/ploytec/ploytec_protocol.h"
#include "common/devices/ploytec/ploytec_codec.h"
#include <stdlib.h>
#include <string.h>

static uint32_t seed = 1;

static void fill_random(struct fake_xfer *x, void *arg)
{
	uint32_t i;

	for (i = 0; i < x->size; i++)
		x->buf[i] = (uint8_t)fake_rand(&seed);
}

int main(void)
{
	struct ozzy_device model = *ozzy_device_lookup(0x0A4A, 0xFFDB);
	static struct ozzy_shared_memory shm;
	static uint8_t out_ring[160 * 8 * 3], in_ring[160 * 8 * 3];
	struct ozzy_pcm_ring ro = { out_ring, 160, 8, 0 }, ri = { in_ring, 160, 8, 0 };
	struct fake_bus fb;
	struct fake_xfer x;
	struct ozzy_engine *e;
	unsigned int i, f, outs = 0, ins = 0, wrong_out = 0, wrong_in = 0;

	model.quirks |= OZZY_QUIRK_OUT_ROUTING | OZZY_QUIRK_IN_ROUTING;
	for (i = 0; i < sizeof(out_ring); i++)
		out_ring[i] = (uint8_t)(i * 7 + 3);
	fake_bus_init(&fb, &shm, true, NULL);
	e = ozzy_engine_init(malloc(ozzy_engine_size(&model)), &model);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_OUT, 0x05, 3, 964, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_IN, 0x86, 3, 1024, 1);
	ozzy_engine_attach(e, &fb.bus, &shm);

	/* Rate requests: a listed rate, an unlisted one (the highest), a restart */
	ozzy_engine_set_rate(e, 48000);
	CHECK(ozzy_engine_start(e), "start");
	CHECK(shm.audio.sample_rate == 48000, "48 kHz requested, runs at %u", (unsigned)shm.audio.sample_rate);
	ozzy_engine_suspend(e);
	fb.head = fb.tail;   /* the platform aborts what is in flight */
	ozzy_engine_set_rate(e, 12345);
	CHECK(ozzy_engine_start(e), "restart");
	CHECK(shm.audio.sample_rate == 96000, "unlisted rate, runs at %u", (unsigned)shm.audio.sample_rate);
	ozzy_engine_suspend(e);
	fb.head = fb.tail;
	ozzy_engine_set_rate(e, 48000);
	CHECK(ozzy_engine_start(e), "restart at 48 kHz");

	/* Outputs 1-2 from playback 3-4, capture 1-2 from inputs 5-6 */
	ozzy_engine_set_setting(e, OZZY_SETTING_OUT_ROUTE_1, 1);
	ozzy_engine_set_setting(e, OZZY_SETTING_IN_ROUTE_1, 2);
	CHECK(!e->out_route.identity && !e->in_route.identity, "routes set");
	ozzy_engine_set_pcm_ring(e, OZZY_PCM_OUT, &ro);
	ozzy_engine_set_pcm_ring(e, OZZY_PCM_IN, &ri);
	for (i = 0; i < 200 && fake_bus_pop(&fb, &x); i++) {
		fake_bus_complete(&fb, e, &x, fill_random, NULL);
		outs += x.pipe == 0x05;
		ins += x.pipe == 0x86;
	}

	/* Packets 4.. were encoded from the ring, from frame 0 on */
	for (i = 4; i < 4 + outs && i < OZZY_NUM_PACKETS; i++) {
		const uint8_t *pkt = (const uint8_t *)shm.audio.output_buffer + i * OZZY_MAX_PACKET_SIZE;

		for (f = 0; f < PLOYTEC_FRAMES_PER_PKT; f++) {
			uint32_t r = ((i - 4) * 80 + f) % 160;
			uint8_t routed[24], enc[48];

			memcpy(routed, out_ring + r * 24, 24);
			memcpy(routed, out_ring + r * 24 + 6, 6);
			ploytec_encode_frame_8ch(enc, routed);
			wrong_out += memcmp(enc, pkt + ploytec_out_frame_offset(PLOYTEC_OUT_LAYOUT_INT, f), 48) != 0;
		}
	}
	/* The ring holds the last two IN packets */
	for (i = 0; i < 160 && ins >= 2; i++) {
		unsigned int j = ins - 2 + i / 80;
		const uint8_t *pkt = (const uint8_t *)shm.audio.input_buffer + (j % OZZY_NUM_PACKETS) * OZZY_MAX_PACKET_SIZE +
				     (i % 80) * PLOYTEC_IN_FRAME_SIZE;
		uint8_t dec[24];

		ploytec_decode_frame_8ch(dec, pkt);
		wrong_in += memcmp(in_ring + i * 24, dec + 12, 6) || memcmp(in_ring + i * 24 + 6, dec + 6, 18);
	}
	CHECK(outs > 8 && ins > 8, "%u out, %u in completions", outs, ins);
	CHECK(!wrong_out, "%u of the encoded output frames aren't the routed ring frames", wrong_out);
	CHECK(!wrong_in, "%u of the captured frames aren't the routed decoded frames", wrong_in);
	free(e);
	return CHECK_DONE("routing");
}
