/* SPDX-License-Identifier: MIT */
/*
 * The US-366 path (isochronous in and out, ploytec_us3xx_engine): every input frame must reach
 * the HAL's plain ring (macOS) or the attached ring (Linux, Windows) in order, a failed packet as
 * its nominal frames of silence; the handshake must be the Windows driver's, with 'M' and 'B' at
 * the first start only (and 'B' only at 48 kHz); a mixer mode with other channels must refuse to
 * start; MIDI out must go in whole event packets, as many as the endpoint takes.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "fake_bus.h"
#include "check.h"
#include "engine/devices/ploytec/ploytec_us3xx_engine.h"
#include "common/devices/ploytec/ploytec_handshake.h"
#include <stdlib.h>
#include <string.h>

#define FRAME     12          /* 4 in, S24_3LE */
#define RING      10240       /* OZZY_PLAIN_RING_FRAMES */

static uint32_t seed = 11;
#define EXPECT    60000
static uint8_t expect[EXPECT * FRAME];
static bool overflow;
static uint64_t expect_frames;
static uint32_t packet_no;
static bool all_fail;

/* PCM in packets: 6 frames (48 kHz), every 7th one short, every 13th failed */
static void fill(struct fake_xfer *x, void *arg)
{
	uint32_t i, k;

	(void)arg;
	if (!x->isoc) {
		memset(x->buf, 0, x->size);
		return;
	}
	for (k = 0; k < x->count; k++) {
		uint8_t *b = x->buf + x->packets[k].offset;
		uint32_t n = packet_no++;
		uint32_t frames = 6 - (n % 7 == 2);

		if (all_fail || n % 13 == 4) {
			x->packets[k].status = -71;
			x->packets[k].actual = 0;
			if (all_fail)
				continue;
			if (expect_frames + 6 > EXPECT) {
				overflow = true;
				continue;
			}
			memset(expect + expect_frames * FRAME, 0, 6 * FRAME);
			expect_frames += 6;
			continue;
		}
		x->packets[k].actual = frames * FRAME;
		x->packets[k].status = 0;
		for (i = 0; i < frames * FRAME; i++)
			b[i] = (uint8_t)fake_rand(&seed);
		if (expect_frames + frames > EXPECT) {
			overflow = true;
			continue;
		}
		memcpy(expect + expect_frames * FRAME, b, frames * FRAME);
		expect_frames += frames;
	}
}

static struct ozzy_engine *make(struct fake_bus *fb, struct ozzy_shared_memory *shm, bool rings, FILE *trace)
{
	const struct ozzy_device *model = ozzy_device_lookup(0x0644, 0x8041);
	struct ozzy_engine *e;

	fake_bus_init(fb, shm, rings, trace);
	fb->i_reg[0] = 0x02;   /* the capture: selector 0x02, channel config 0x0F */
	fb->i_reg[1] = 0x0F;
	e = ozzy_engine_init(malloc(ozzy_engine_size(model)), model);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_OUT, 0x02, 1, 512, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_IN, 0x81, 1, 512, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_MIDI_IN, 0x83, 2, 512, 0);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_MIDI_OUT, 0x04, 3, 16, 4);
	ozzy_engine_attach(e, &fb->bus, shm);
	ozzy_engine_set_anchors(e, !rings);
	return e;
}

/* Complete @n transfers (MIDI IN reads go back unanswered) */
static void pump(struct fake_bus *fb, struct ozzy_engine *e, unsigned int n)
{
	struct fake_xfer x;

	while (n && fake_bus_pop(fb, &x)) {
		if (x.pipe == 0x83) {
			fake_bus_push(fb, &x);
			continue;
		}
		fake_bus_complete(fb, e, &x, fill, NULL);
		n--;
	}
}

/* The control requests of a trace, one "type req value index" per line */
static unsigned int count_requests(FILE *t, const char *what)
{
	char line[512];
	unsigned int n = 0;

	rewind(t);
	while (fgets(line, sizeof(line), t))
		if (!strncmp(line, "control ", 8) && strstr(line, what))
			n++;
	return n;
}

/* ozzy_isoc_input alone on a ring of 100 frames, which packets of 5 and 6 frames straddle */
static void test_small_ring(void)
{
	static struct ozzy_shared_memory shm;
	static uint8_t ring[100 * FRAME], xfers[ISOC_IN_TRANSFERS][16 * 512];
	struct ozzy_isoc_input_config c;
	struct ozzy_isoc_input in;
	struct fake_bus fb;
	struct fake_xfer x;
	uint64_t f, wrong = 0;
	unsigned int i, done = 0;

	expect_frames = 0;
	packet_no = 0;
	fake_bus_init(&fb, &shm, false, NULL);
	memset(&c, 0, sizeof(c));
	c.rate = 48000;
	c.frame_bytes = FRAME;
	c.in_addr = 0x81;
	c.in_max_packet = 512;
	c.in_interval = 1;
	for (i = 0; i < ISOC_IN_TRANSFERS; i++)
		c.xfer[i] = xfers[i];
	c.xfer_size = sizeof(xfers[0]);
	c.ring = ring;
	c.ring_frames = 100;
	CHECK(ozzy_isoc_input_start(&in, &fb.bus, &c), "input start");
	while (done++ < 200 && fake_bus_pop(&fb, &x)) {
		fill(&x, NULL);
		ozzy_isoc_input_complete(&in, x.pipe, x.xfer, 0, x.packets, x.count);
	}
	CHECK(!overflow && in.in_frame == expect_frames, "frames %llu, expected %llu", (unsigned long long)in.in_frame,
	      (unsigned long long)expect_frames);
	for (f = expect_frames - 100; f < expect_frames; f++)
		wrong += memcmp(ring + (f % 100) * FRAME, expect + f * FRAME, FRAME) != 0;
	CHECK(wrong == 0, "%llu of the last 100 frames wrong in a 100-frame ring", (unsigned long long)wrong);
}

static void test_hal_ring(void)
{
	static struct ozzy_shared_memory shm;
	struct fake_bus fb;
	struct ozzy_engine *e;
	uint64_t f, wrong = 0;

	expect_frames = 0;
	packet_no = 0;
	e = make(&fb, &shm, false, NULL);
	CHECK(ozzy_engine_start(e), "start (no rings)");
	CHECK(shm.audio.device_flags == (OZZY_DEVICE_FLAG_PLAIN_OUTPUT | OZZY_DEVICE_FLAG_PLAIN_INPUT) &&
	      shm.audio.input_ring_frames == RING && shm.audio.input_bytes_per_frame == FRAME &&
	      shm.audio.output_bytes_per_frame == 18 && shm.audio.sample_rate == 48000,
	      "layout: flags %x ring %u frame %u/%u rate %u", (unsigned int)shm.audio.device_flags,
	      shm.audio.input_ring_frames, shm.audio.input_bytes_per_frame, shm.audio.output_bytes_per_frame,
	      (unsigned int)shm.audio.sample_rate);
	CHECK(shm.audio.input_safety_offset_frames >= 16 * 6, "input safety offset %u", shm.audio.input_safety_offset_frames);
	/* Past the ring's end: ~2.5 times round it (an IN transfer brings ~94 frames) */
	pump(&fb, e, 500);
	CHECK(!overflow, "the test's expectation overflowed");
	CHECK(expect_frames > RING + RING / 2, "only %llu frames", (unsigned long long)expect_frames);
	for (f = expect_frames > RING ? expect_frames - RING : 0; f < expect_frames; f++)
		wrong += memcmp(shm.audio.input_buffer + (f % RING) * FRAME, expect + f * FRAME, FRAME) != 0;
	CHECK(wrong == 0, "%llu of the last %u frames wrong in the HAL's ring", (unsigned long long)wrong, RING);
	CHECK(((struct ploytec_us3xx_engine *)e)->input.in_frame == expect_frames, "frame count %llu, expected %llu",
	      (unsigned long long)((struct ploytec_us3xx_engine *)e)->input.in_frame, (unsigned long long)expect_frames);
	ozzy_engine_stop(e);
	free(e);
}

static void test_attached_ring(void)
{
	static struct ozzy_shared_memory shm;
	static uint8_t in_ring[1000 * FRAME];
	struct ozzy_pcm_ring ri = { in_ring, 1000, 4, 0 };
	struct fake_bus fb;
	struct ozzy_engine *e;
	uint64_t f, wrong = 0;

	expect_frames = 0;
	packet_no = 0;
	e = make(&fb, &shm, true, NULL);
	ozzy_engine_set_pcm_ring(e, OZZY_PCM_IN, &ri);
	CHECK(ozzy_engine_start(e), "start (rings)");
	pump(&fb, e, 300);
	CHECK(!overflow, "the test's expectation overflowed");
	CHECK(fb.progress[OZZY_PCM_IN] == expect_frames, "progress %llu, frames %llu",
	      (unsigned long long)fb.progress[OZZY_PCM_IN], (unsigned long long)expect_frames);
	for (f = expect_frames - 1000; f < expect_frames; f++)
		wrong += memcmp(in_ring + (f % 1000) * FRAME, expect + f * FRAME, FRAME) != 0;
	CHECK(wrong == 0, "%llu of the last 1000 frames wrong in the attached ring", (unsigned long long)wrong);
	ozzy_engine_stop(e);
	free(e);
}

/* 44.1 kHz with every packet lost: silence of 5 or 6 frames each, 44100 a second */
static void test_nominal_silence(void)
{
	static struct ozzy_shared_memory shm;
	struct fake_bus fb;
	struct ozzy_engine *e;
	struct ploytec_us3xx_engine *ue;
	uint64_t in_xfers = 0;
	struct fake_xfer x;

	all_fail = true;
	packet_no = 0;
	e = make(&fb, &shm, false, NULL);
	ue = (struct ploytec_us3xx_engine *)e;
	ozzy_engine_set_rate(e, 44100);
	CHECK(ozzy_engine_start(e) && shm.audio.sample_rate == 44100, "start at 44.1 kHz");
	while (in_xfers < 500 && fake_bus_pop(&fb, &x)) {
		if (x.pipe == 0x83) {
			fake_bus_push(&fb, &x);
			continue;
		}
		in_xfers += x.pipe == 0x81;
		fake_bus_complete(&fb, e, &x, fill, NULL);
	}
	/* 500 transfers of 16 microframes: 8000 microframes, one second */
	CHECK(ue->input.in_frame == 44100, "44.1 kHz silence: %llu frames in a second", (unsigned long long)ue->input.in_frame);
	all_fail = false;
	ozzy_engine_stop(e);
	free(e);
}

static void test_handshake(void)
{
	static struct ozzy_shared_memory shm;
	struct fake_bus fb;
	struct ozzy_engine *e;
	FILE *t = tmpfile();

	CHECK(t != NULL, "tmpfile");
	if (!t)
		return;
	/* First start at 48 kHz: the selector written with bits 3-4 (0x02 -> 0x0A), 'M' and 'B', six
	 * SET_CURs, the five S/PDIF registers */
	e = make(&fb, &shm, false, t);
	CHECK(ozzy_engine_start(e), "start");
	CHECK(count_requests(t, "40 49 000a 0000") == 1, "selector write 0x0A");
	CHECK(count_requests(t, "40 4d 0000 0000 64") == 1 && count_requests(t, "40 42 0000 0000 256") == 1, "'M' and 'B'");
	CHECK(count_requests(t, "22 01 0100 0081 3") == 3 && count_requests(t, "22 01 0100 0002 3") == 3, "three SET_CUR pairs");
	CHECK(count_requests(t, "40 41 0e00 0101") == 1 && count_requests(t, "40 41 1002 0101") == 1, "the S/PDIF registers");
	/* A restart at 96 kHz: no 'M' or 'B' again, the rate code 0x0A */
	CHECK(ozzy_engine_change_rate(e, 96000) == OZZY_SETTING_RESTART && ozzy_engine_restart(e), "restart at 96 kHz");
	CHECK(count_requests(t, "40 4d") == 1 && count_requests(t, "40 42") == 1, "'M' / 'B' again on a restart");
	CHECK(count_requests(t, "40 41 100a 0101") == 1 && shm.audio.sample_rate == 96000, "96 kHz S/PDIF rate code");
	ozzy_engine_stop(e);
	free(e);
	fclose(t);

	/* A first start at 96 kHz: 'M' and the same 'B' as at 48 kHz (Windows sends it at every rate) */
	t = tmpfile();
	e = make(&fb, &shm, false, t);
	ozzy_engine_set_rate(e, 96000);
	CHECK(ozzy_engine_start(e), "start at 96 kHz");
	CHECK(count_requests(t, "40 4d") == 1 && count_requests(t, "40 42 0000 0000 256") == 1, "96 kHz: 'M' and 'B'");
	ozzy_engine_stop(e);
	free(e);
	fclose(t);

	/* The mixer bypassed (2 in / 2 out): no start */
	e = make(&fb, &shm, false, NULL);
	fb.i_reg[1] = 0x1F;
	CHECK(!ozzy_engine_start(e), "started in a mixer mode with other channels");
	free(e);
}

/* The 'M' block (d-hey's captures): both modes as Tascam sent them, and the routing byte */
static void test_mixer_block(void)
{
	uint8_t b[PLOYTEC_US3XX_MIXER_SIZE];

	CHECK(ploytec_us366_routing(OZZY_US366_MIXER, OZZY_US366_PCM_12, OZZY_US366_PCM_34) == 0x24,
	      "Tascam's default routing is 0x24");
	CHECK(ploytec_us366_routing(OZZY_US366_PCM_12, OZZY_US366_PCM_12, OZZY_US366_PCM_34) == 0x25 &&
	      ploytec_us366_routing(OZZY_US366_PCM_12, OZZY_US366_PCM_34, OZZY_US366_PCM_34) == 0x29 &&
	      ploytec_us366_routing(OZZY_US366_PCM_12, OZZY_US366_PCM_34, OZZY_US366_PCM_56) == 0x39,
	      "the panel's three changes in the capture: 0x25, 0x29, 0x39");
	ploytec_us366_mixer_block(b, 0x0F, 0x24);
	CHECK(b[8] == 0 && b[9] == 0 && b[26] == 0 && b[27] == 0 && b[56] == 0x24 && b[32] == 0x7F,
	      "digital out: the captured block");
	ploytec_us366_mixer_block(b, 0x03, 0x24);
	CHECK(b[8] == 0x5A && b[9] == 0x80 && b[26] == 0x5A && b[27] == 0x80 && b[56] == 0x34 && b[32] == 0x7F,
	      "digital in: the captured block, whatever the routing");
}

/* Digital in on the front panel: 6 in / 4 out after a probe; a routing change rewrites 'M' */
static void test_front_panel(void)
{
	static struct ozzy_shared_memory shm;
	struct fake_bus fb;
	struct ozzy_engine *e;
	struct ploytec_us3xx_engine *ue;
	FILE *t = tmpfile();

	CHECK(t != NULL, "tmpfile");
	if (!t)
		return;
	e = make(&fb, &shm, false, t);
	ue = (struct ploytec_us3xx_engine *)e;
	fb.i_reg[0] = 0x06;   /* the input capture: selector 0x06, config 0x03 */
	fb.i_reg[1] = 0x03;
	CHECK(ozzy_engine_probe(e) && ozzy_engine_channels(e, OZZY_PCM_IN) == 6 &&
	      ozzy_engine_channels(e, OZZY_PCM_OUT) == 4, "probe: 6 in / 4 out with digital in");
	CHECK(ozzy_engine_start(e), "start with digital in");
	CHECK(shm.audio.input_channels == 6 && shm.audio.output_channels == 4 &&
	      shm.audio.input_bytes_per_frame == 18 && shm.audio.output_bytes_per_frame == 12,
	      "the HAL gets 6 in / 4 out");
	CHECK(ue->mixer_last[56] == 0x34 && ue->mixer_last[8] == 0x5A, "Tascam's digital-in 'M' block");
	CHECK(count_requests(t, "40 49") == 0, "no selector write (0x06 already has config bits 3-4 clear)");
	/* The switch flipped since the probe: the start refuses until a replug */
	fb.i_reg[1] = 0x0F;
	CHECK(ozzy_engine_change_rate(e, 96000) == OZZY_SETTING_RESTART && !ozzy_engine_restart(e),
	      "a restart refuses a mode the PCM devices weren't made for");
	ozzy_engine_stop(e);
	free(e);
	fclose(t);

	/* Digital out: a routing change sends 'M' again with the new byte 56, without 'B' */
	t = tmpfile();
	e = make(&fb, &shm, false, t);
	ue = (struct ploytec_us3xx_engine *)e;
	CHECK(ozzy_engine_probe(e) && ozzy_engine_channels(e, OZZY_PCM_OUT) == 6, "probe: 4 in / 6 out");
	CHECK(ozzy_engine_start(e) && ue->mixer_last[56] == 0x24, "start: Tascam's routing 0x24");
	CHECK(ozzy_engine_change_setting(e, OZZY_SETTING_US366_DIGITAL, OZZY_US366_PCM_56) == OZZY_SETTING_RESTART &&
	      ozzy_engine_restart(e), "the digital out to PCM 5-6");
	CHECK(ue->mixer_last[56] == 0x2C && count_requests(t, "40 4d") == 2 && count_requests(t, "40 42") == 1,
	      "'M' again with 0x2C (mixer, PCM 5-6, PCM 3-4), 'B' only once");
	CHECK(ozzy_engine_change_rate(e, 96000) == OZZY_SETTING_RESTART && ozzy_engine_restart(e) &&
	      count_requests(t, "40 4d") == 2, "a rate change with the same routing: no 'M'");
	ozzy_engine_stop(e);
	free(e);
	fclose(t);
}

static void test_midi_out(void)
{
	static struct ozzy_shared_memory shm;
	static const uint8_t notes[] = { 0x90, 60, 100, 61, 100, 62, 100, 63, 100, 64, 100 };   /* running status */
	struct fake_bus fb;
	struct ozzy_engine *e;
	struct fake_xfer x;
	uint32_t sizes[4], n = 0;

	e = make(&fb, &shm, false, NULL);
	CHECK(ozzy_engine_start(e), "start");
	while (fake_bus_pop(&fb, &x))
		;   /* the streaming transfers: not completed here */
	ozzy_midi_ring_push(&shm.midi_out, notes, sizeof(notes));
	ozzy_engine_midi_out_pending(e);
	while (n < 4 && fake_bus_pop(&fb, &x)) {
		if (x.pipe != 0x04)
			continue;
		sizes[n] = x.size;
		CHECK(x.buf[0] == 0x09 && x.buf[1] == 0x90 && x.buf[2] == 60 + n * 4, "packet %02x %02x %02x", x.buf[0],
		      x.buf[1], x.buf[2]);
		n++;
		fake_bus_complete(&fb, e, &x, NULL, NULL);
	}
	CHECK(n == 2 && sizes[0] == 16 && sizes[1] == 4, "MIDI out transfers: %u, %u and %u bytes", n, n ? sizes[0] : 0,
	      n > 1 ? sizes[1] : 0);
	ozzy_engine_stop(e);
	free(e);
}

int main(void)
{
	test_small_ring();
	test_hal_ring();
	test_attached_ring();
	test_nominal_silence();
	test_handshake();
	test_mixer_block();
	test_front_panel();
	test_midi_out();
	return CHECK_DONE("isochronous input");
}
