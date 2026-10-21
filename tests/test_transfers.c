/* SPDX-License-Identifier: MIT */
/*
 * PCM transfer sizes (OZZY_SETTING_PCM_OUT_FRAMES / _IN_FRAMES) on the Ploytec bulk / interrupt
 * family, both out layouts (DB4: 964-byte interrupt, ESU1808: 512-byte bulk), every size that is
 * whole packets of the out endpoint (20 frames on the 964 layout, 10 on 512; the rest is rounded
 * up, ozzy_pcm_step): a transfer is frames / 10 sub-packets out and frames * 64 bytes in, end to end in the shared
 * memory's rings, which hold a whole number of them; the bytes on the wire are the same stream
 * at every size (PCM, MIDI pacing, control bytes), only cut differently, from the first
 * transfer the ring is encoded into (the ones queued at start are silence: 4 transfers' worth,
 * so it starts later at larger sizes); input decodes to the same ring; the safety offsets
 * follow the sizes; the clock anchors on a whole number of transfers near 3200 frames. Then
 * the queues (OZZY_SETTING_PCM_OUT_QUEUE / _IN_QUEUE), 2 to 4 transfers in flight at the
 * smallest sizes: the same stream, safety offsets of queue x frames.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "fake_bus.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>

#define PCM_OUT  0x05
#define PCM_IN   0x86
#define FRAMES   960                /* compared, out and in */
#define SUBPKTS  (FRAMES / 10)
#define IN_RING  (FRAMES + 200)     /* room for the last in transfer past FRAMES */

struct result {
	uint8_t wire[SUBPKTS * 512];    /* out sub-packets from the first encoded transfer */
	uint32_t wire_bytes;
	uint8_t in_ring[IN_RING * 16 * 3];
	uint64_t anchor;                /* the clock's last anchor */
	uint32_t anchor_frames;         /* its spacing */
	uint32_t in_frame_bytes;        /* bytes of a frame in in_ring */
	uint32_t safety_out, safety_in;
	uint32_t rate;
	uint32_t transfer_frames[2];
};

struct in_fill {
	uint32_t counter;
};

/* The device's input: one byte stream, whatever the transfers cut it into */
static void fill_in(struct fake_xfer *x, void *arg)
{
	struct in_fill *st = arg;
	uint32_t i;

	if (x->pipe != PCM_IN) {
		memset(x->buf, 0xFD, x->size);   /* MIDI IN: idle */
		return;
	}
	for (i = 0; i < x->size; i++)
		x->buf[i] = (uint8_t)(st->counter++ * 37 + 11);
}

static int run_io_buffer = -1;   /* OZZY_SETTING_IO_BUFFER_MAX for run(), -1: the default */

static void run(uint16_t vid, uint16_t pid, uint16_t out_max_packet, unsigned int out_value, unsigned int in_value,
		unsigned int queue_value, struct result *res)
{
	uint32_t queued = ozzy_pcm_queue(queue_value);
	const struct ozzy_device *model = ozzy_device_lookup(vid, pid);
	static struct ozzy_shared_memory shm;
	static uint8_t out_ring[FRAMES * 16 * 3];
	struct ozzy_pcm_ring ro = { out_ring, FRAMES, model->out_channels, 0 };
	struct ozzy_pcm_ring ri = { res->in_ring, IN_RING, model->in_channels, 0 };
	uint32_t out_frames = ozzy_pcm_frames(false, out_value), in_frames = ozzy_pcm_frames(true, in_value);
	uint32_t out_bytes, in_bytes;
	uint32_t subpkt = out_max_packet <= 512 ? 512 : 482;
	uint32_t out_done = 0, skip = queued * out_frames / 10, k;
	uint8_t midi[64];
	struct in_fill st = { 0 };
	struct fake_bus fb;
	struct fake_xfer x;
	struct ozzy_engine *e;

	memset(&shm, 0, sizeof(shm));
	memset(res, 0, sizeof(*res));
	for (k = 0; k < sizeof(out_ring); k++)
		out_ring[k] = (uint8_t)(k * 13 + 5);
	fake_bus_init(&fb, &shm, true, NULL);
	e = ozzy_engine_init(malloc(ozzy_engine_size(model)), model);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_OUT, PCM_OUT, out_max_packet <= 512 ? 2 : 3, out_max_packet, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_IN, PCM_IN, 2, 512, 0);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_MIDI_IN, 0x83, 3, 64, 1);
	ozzy_engine_attach(e, &fb.bus, &shm);
	ozzy_engine_set_anchors(e, true);   /* as the macOS kext */
	CHECK(ozzy_engine_has_setting(e, OZZY_SETTING_PCM_OUT_FRAMES) &&
	      ozzy_engine_has_setting(e, OZZY_SETTING_PCM_IN_FRAMES), "%s: no transfer size settings", model->name);
	CHECK(ozzy_engine_change_setting(e, OZZY_SETTING_PCM_OUT_FRAMES, out_value) != OZZY_SETTING_INVALID &&
	      ozzy_engine_change_setting(e, OZZY_SETTING_PCM_IN_FRAMES, in_value) != OZZY_SETTING_INVALID,
	      "%s: sizes refused", model->name);
	CHECK(ozzy_engine_change_setting(e, OZZY_SETTING_PCM_OUT_QUEUE, queue_value) != OZZY_SETTING_INVALID &&
	      ozzy_engine_change_setting(e, OZZY_SETTING_PCM_IN_QUEUE, queue_value) != OZZY_SETTING_INVALID,
	      "%s: queues refused", model->name);
	if (run_io_buffer >= 0)
		CHECK(ozzy_engine_change_setting(e, OZZY_SETTING_IO_BUFFER_MAX, (unsigned int)run_io_buffer) !=
		      OZZY_SETTING_INVALID, "%s: IO buffer refused", model->name);
	for (k = 0; k < sizeof(midi); k++)
		midi[k] = (uint8_t)(0x80 | k);
	ozzy_midi_ring_push(&shm.midi_out, midi, sizeof(midi));
	ozzy_engine_set_pcm_ring(e, OZZY_PCM_OUT, &ro);
	ozzy_engine_set_pcm_ring(e, OZZY_PCM_IN, &ri);
	CHECK(ozzy_engine_start(e), "%s: start", model->name);
	res->safety_out = shm.audio.safety_offset_frames;
	res->safety_in = shm.audio.input_safety_offset_frames;
	res->rate = shm.audio.sample_rate;
	res->transfer_frames[0] = shm.info.transfer_frames[OZZY_PCM_OUT];
	res->transfer_frames[1] = shm.info.transfer_frames[OZZY_PCM_IN];
	/* What runs: a size that isn't whole packets is rounded up */
	out_frames = res->transfer_frames[0];
	in_frames = res->transfer_frames[1];
	skip = queued * out_frames / 10;
	res->anchor_frames = shm.audio.timestamp_frames;
	res->in_frame_bytes = model->in_channels * 3;
	out_bytes = out_frames / 10 * subpkt;
	in_bytes = in_frames * 64;
	CHECK(shm.audio.output_ring_frames == sizeof(shm.audio.output_buffer) / out_bytes * out_frames &&
	      shm.audio.input_ring_frames == sizeof(shm.audio.input_buffer) / in_bytes * in_frames,
	      "%s %u/%u: rings of %u / %u frames", model->name, out_frames, in_frames, shm.audio.output_ring_frames,
	      shm.audio.input_ring_frames);
	/* Out until the clock passes its first anchor and the wire has FRAMES frames; in until
	 * the ring is full once */
	while (fake_bus_pop(&fb, &x)) {
		if (x.pipe == PCM_OUT) {
			uint32_t at = (uint32_t)(x.buf - (uint8_t *)shm.audio.output_buffer);

			CHECK(x.size == out_bytes, "%s: out transfer of %u bytes for %u frames", model->name, x.size,
			      out_frames);
			CHECK(at % out_bytes == 0 && at + out_bytes <= sizeof(shm.audio.output_buffer),
			      "%s: out transfer at %u, not end to end", model->name, at);
			/* Encoded at submit: its bytes are final now */
			for (k = 0; k < x.size / subpkt; k++, skip ? skip-- : 0) {
				if (skip || res->wire_bytes >= sizeof(res->wire) / 512 * subpkt)
					continue;
				memcpy(res->wire + res->wire_bytes, x.buf + k * subpkt, subpkt);
				res->wire_bytes += subpkt;
			}
			out_done += out_frames;
			if (out_done > res->anchor_frames + queued * out_frames && res->wire_bytes >= SUBPKTS * subpkt &&
			    e->pcm_pos[OZZY_PCM_IN] >= FRAMES)
				break;
		} else if (x.pipe == PCM_IN) {
			uint32_t at = (uint32_t)(x.buf - (uint8_t *)shm.audio.input_buffer);

			CHECK(x.size == in_bytes && at % in_bytes == 0, "%s: in transfer of %u bytes at %u for %u frames",
			      model->name, x.size, at, in_frames);
			if (e->pcm_pos[OZZY_PCM_IN] >= FRAMES)
				continue;   /* the ring is full: leave it as it is */
		}
		fake_bus_complete(&fb, e, &x, fill_in, &st);
	}
	res->anchor = shm.audio.timestamp.sample_time;
	ozzy_engine_stop(e);
	free(e);
}

static void compare(const char *name, uint16_t vid, uint16_t pid, uint16_t out_max_packet)
{
	static struct result ref, got;
	/* Out transfers are whole packets: 48 bytes a frame (PLOYTEC_OUT_FRAME_SIZE) */
	uint32_t out_step = ozzy_pcm_step(out_max_packet <= 512 ? 2 : 3, out_max_packet, 48);
	unsigned int small = out_step / 10 - 1, io = ozzy_setting_value_fits(OZZY_SETTING_PCM_OUT_FRAMES, 2, out_step, 0) ? 2 : 3;
	uint32_t small_frames = ozzy_pcm_frames(false, small), io_frames = ozzy_pcm_frames(false, io);
	unsigned int v;

	/* The default (80 / 80), then every out size, with the in sizes alongside */
	run(vid, pid, out_max_packet, ozzy_setting_descs[OZZY_SETTING_PCM_OUT_FRAMES].def,
	    ozzy_setting_descs[OZZY_SETTING_PCM_IN_FRAMES].def, OZZY_NUM_PCM_QUEUES - 1, &ref);
	CHECK(ref.safety_out == 4 * 80 && ref.safety_in == 4 * 80 && ref.anchor_frames == 3200,
	      "%s: default safety offsets %u / %u, anchors %u", name, ref.safety_out, ref.safety_in, ref.anchor_frames);
	for (v = 0; v < OZZY_NUM_PCM_IN_FRAMES; v++) {
		unsigned int ov = v < OZZY_NUM_PCM_OUT_FRAMES ? v : OZZY_NUM_PCM_OUT_FRAMES - 1;
		uint32_t out_frames = ozzy_pcm_frames(false, ov), in_frames = ozzy_pcm_frames(true, v);

		if (out_frames % out_step) {
			/* Not whole packets: the engine takes the next size up */
			run(vid, pid, out_max_packet, ov, v, OZZY_NUM_PCM_QUEUES - 1, &got);
			CHECK(got.transfer_frames[0] == (out_frames / out_step + 1) * out_step,
			      "%s: %u out frames ran as %u", name, out_frames, got.transfer_frames[0]);
			continue;
		}
		run(vid, pid, out_max_packet, ov, v, OZZY_NUM_PCM_QUEUES - 1, &got);
		CHECK(got.transfer_frames[0] == out_frames && got.transfer_frames[1] == in_frames,
		      "%s: info says %u / %u frames per transfer", name, got.transfer_frames[0], got.transfer_frames[1]);
		/* What is queued, at least the jitter floor at the rate */
		CHECK(got.safety_out == ozzy_engine_safety_offset(4 * out_frames, got.rate) &&
		      got.safety_in == ozzy_engine_safety_offset(4 * in_frames, got.rate),
		      "%s %u/%u: safety offsets %u / %u", name, out_frames, in_frames, got.safety_out, got.safety_in);
		CHECK(got.wire_bytes == ref.wire_bytes && !memcmp(got.wire, ref.wire, ref.wire_bytes),
		      "%s %u/%u: the wire differs from 80-frame transfers", name, out_frames, in_frames);
		CHECK(!memcmp(got.in_ring, ref.in_ring, FRAMES * ref.in_frame_bytes), "%s %u/%u: input decodes differently",
		      name, out_frames, in_frames);
		CHECK(got.anchor_frames % out_frames == 0 && got.anchor_frames >= 3200 &&
		      got.anchor_frames < 3200 + out_frames && got.anchor && got.anchor % got.anchor_frames == 0,
		      "%s %u/%u: clock anchor at %llu, every %u", name, out_frames, in_frames,
		      (unsigned long long)got.anchor, got.anchor_frames);
	}
	/* The largest IO buffer sets the anchors: 8/3 of it in whole out transfers (at 30 frames:
	 * 300 -> 810, 600 -> 1620, 1200 -> 3210, 2400 -> 6420; 40 on the 964 layout), in 24 */
	for (v = 0; v < OZZY_NUM_IO_BUFFERS; v++) {
		run_io_buffer = (int)v;
		run(vid, pid, out_max_packet, io, 2, OZZY_NUM_PCM_QUEUES - 1, &got);
		CHECK(got.anchor_frames == ozzy_anchor_frames(v, io_frames) && got.anchor_frames % io_frames == 0 &&
		      got.anchor && got.anchor % got.anchor_frames == 0, "%s IO buffer %u: anchors every %u, at %llu",
		      name, ozzy_io_buffer_frames(v), got.anchor_frames, (unsigned long long)got.anchor);
	}
	run_io_buffer = -1;
	/* The queues at the smallest sizes: one packet out (10 or 20 frames), 8 in (one in flight
	 * isn't offered) */
	for (v = OZZY_PCM_QUEUE_MIN; v < OZZY_NUM_PCM_QUEUES; v++) {
		uint32_t q = ozzy_pcm_queue(v);

		run(vid, pid, out_max_packet, small, 0, v, &got);
		CHECK(got.safety_out == ozzy_engine_safety_offset(q * small_frames, got.rate) &&
		      got.safety_in == ozzy_engine_safety_offset(q * 8, got.rate),
		      "%s %u/8, %u queued: safety offsets %u / %u",
		      name, small_frames, q, got.safety_out, got.safety_in);
		CHECK(got.wire_bytes == ref.wire_bytes && !memcmp(got.wire, ref.wire, ref.wire_bytes),
		      "%s %u/8, %u queued: the wire differs", name, small_frames, q);
		CHECK(!memcmp(got.in_ring, ref.in_ring, FRAMES * ref.in_frame_bytes), "%s %u/8, %u queued: input differs",
		      name, small_frames, q);
	}
}

int main(void)
{
	compare("Xone:DB4 (964 interrupt)", 0x0A4A, 0xFFDB, 964);
	compare("ESI ESU1808 (512 bulk)", 0x0A92, 0x0111, 512);
	return CHECK_DONE("transfers");
}
