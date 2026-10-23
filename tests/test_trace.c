/* SPDX-License-Identifier: MIT */
/*
 * Golden trace: what the engine does on the bus for four devices, one per path:
 *   Xone:DB4     interrupt layout, PCM rings attached at packet 20, detached at 300
 *   ESI ESU1808  bulk layout, the S/PDIF mirror (pair 2), the same rings
 *   Akai EIE Pro isochronous out with feedback, MIDI out on the 1 ms timer
 *   PHASE X64    the same path, with its WM8776 writes (inputs 1-2 on Phono)
 *   Xone:1D      USB MIDI
 * with MIDI going out every 5 completions and MIDI IN bytes coming in. Every submission,
 * control request and progress report is printed, then hashes of the shared memory and the
 * rings. Compared with golden/trace.txt by run.sh; after an intended change: make golden.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "fake_bus.h"
#include <stdlib.h>
#include <string.h>

#define MIDI_IN 0x83

struct fill_state {
	uint32_t counter[256];
};

static void fill(struct fake_xfer *x, void *arg)
{
	struct fill_state *st = arg;
	uint32_t i, k;

	if (x->isoc) {
		/* feedback: 12 frames per microframe (96 kHz) in 10.14 */
		for (k = 0; k < x->count; k++) {
			uint8_t *b = x->buf + x->packets[k].offset;

			b[0] = 0x00;
			b[1] = 0x00;
			b[2] = 0x0C;
		}
	} else if (x->pipe == MIDI_IN) {
		/* one MIDI byte per transfer, then idle */
		memset(x->buf, 0xFD, x->size);
		x->buf[0] = (uint8_t)(0x40 + st->counter[MIDI_IN]++ % 32);
	} else {
		for (i = 0; i < x->size; i++)
			x->buf[i] = (uint8_t)(st->counter[x->pipe]++ * 37 + 11);
	}
}

static uint64_t hash(const void *p, size_t n)
{
	const uint8_t *b = p;
	uint64_t h = 1469598103934665603ULL;

	while (n--) {
		h ^= *b++;
		h *= 1099511628211ULL;
	}
	return h;
}

static void run(uint16_t vid, uint16_t pid, int setting, int value, const uint8_t (*eps)[5])
{
	const struct ozzy_device *model = ozzy_device_lookup(vid, pid);
	static struct ozzy_shared_memory shm;
	static uint8_t out_ring[960 * 16 * 3], in_ring[960 * 16 * 3];
	struct ozzy_pcm_ring ro = { out_ring, 960, model->out_channels, 0 };
	struct ozzy_pcm_ring ri = { in_ring, 960, model->in_channels, 0 };
	struct fill_state st;
	struct fake_bus fb;
	struct fake_xfer x;
	struct ozzy_engine *e;
	uint8_t seen[4096];
	uint32_t seen_n = 0, r, w;
	unsigned int done = 0, k;

	memset(&shm, 0, sizeof(shm));
	memset(&st, 0, sizeof(st));
	memset(in_ring, 0, sizeof(in_ring));
	for (k = 0; k < sizeof(out_ring); k++)
		out_ring[k] = (uint8_t)(k * 13 + 5);
	fake_bus_init(&fb, &shm, true, stdout);

	printf("== %s\n", model->name);
	e = ozzy_engine_init(malloc(ozzy_engine_size(model)), model);
	for (k = 0; eps[k][0] != 0xFF; k++)
		ozzy_engine_set_endpoint(e, (enum ozzy_pipe_role)eps[k][0], eps[k][1], eps[k][2],
					 (uint16_t)(eps[k][3] | eps[k][4] << 8), 4);
	ozzy_engine_attach(e, &fb.bus, &shm);
	ozzy_engine_set_anchors(e, true);   /* as the macOS kext */
	if (setting >= 0)
		ozzy_engine_set_setting(e, (unsigned int)setting, (uint8_t)value);
	printf("start %d\n", ozzy_engine_start(e));

	/* 600 completions besides MIDI IN, whose reads complete every 4th turn */
	while (done < 600 && fake_bus_pop(&fb, &x)) {
		bool xone = model->family == OZZY_FAMILY_PLOYTEC_XONE;

		if (x.pipe == MIDI_IN && done % 4) {
			fake_bus_push(&fb, &x);
			done++;
			continue;
		}
		if (x.pipe != MIDI_IN) {
			done++;
			if (done == 20 && xone) {
				ozzy_engine_set_pcm_ring(e, OZZY_PCM_OUT, &ro);
				ozzy_engine_set_pcm_ring(e, OZZY_PCM_IN, &ri);
			}
			if (done == 300 && xone) {
				ozzy_engine_set_pcm_ring(e, OZZY_PCM_OUT, NULL);
				ozzy_engine_set_pcm_ring(e, OZZY_PCM_IN, NULL);
				printf("detached\n");
			}
			if (done % 5 == 0) {
				uint8_t note[3] = { 0x90, (uint8_t)(done & 0x7F), 0x7F };

				ozzy_midi_ring_push(&shm.midi_out, note, 3);
				ozzy_engine_midi_out_pending(e);
			}
			if (fb.timer_on && done % 3 == 0)
				ozzy_engine_timer(e);
		}
		fake_bus_complete(&fb, e, &x, fill, &st);
	}
	ozzy_engine_stop(e);

	r = shm.midi_in.read_index;
	w = shm.midi_in.write_index;
	while (r != w && seen_n < sizeof(seen)) {
		seen[seen_n++] = shm.midi_in.buffer[r];
		r = (r + 1) & OZZY_MIDI_RING_MASK;
	}
	printf("midi_in %u:", seen_n);
	for (k = 0; k < seen_n && k < 24; k++)
		printf(" %02x", seen[k]);
	printf("\nshm %016llx out_ring %016llx in_ring %016llx\n", (unsigned long long)hash(&shm, sizeof(shm)),
	       (unsigned long long)hash(out_ring, sizeof(out_ring)), (unsigned long long)hash(in_ring, sizeof(in_ring)));
	free(e);
}

int main(void)
{
	/* role, address, bmAttributes & 3, wMaxPacketSize (lo, hi) */
	static const uint8_t db4[][5] = { { 0, 0x05, 3, 0xC4, 0x03 }, { 1, 0x86, 3, 0x00, 0x04 }, { 2, 0x83, 3, 0x40, 0 }, { 0xFF } };
	static const uint8_t esu[][5] = { { 0, 0x05, 2, 0x00, 0x02 }, { 1, 0x86, 2, 0x00, 0x02 }, { 2, 0x83, 2, 0x40, 0 }, { 0xFF } };
	static const uint8_t eie[][5] = { { 0, 0x02, 1, 0x00, 0x04 }, { 1, 0x86, 2, 0x00, 0x02 }, { 2, 0x83, 2, 0x40, 0 },
					  { 3, 0x03, 2, 0x40, 0 }, { 4, 0x81, 1, 0x03, 0 }, { 0xFF } };
	static const uint8_t none[][5] = { { 0xFF } };

	run(0x0A4A, 0xFFDB, -1, 0, db4);
	run(0x0A92, 0x0111, OZZY_SETTING_SPDIF_MIRROR, 2, esu);
	run(0x09E8, 0x0010, -1, 0, eie);
	run(0x0A4A, 0x5B01, OZZY_SETTING_WM_INPUT_12, OZZY_WM_IN12_PHONO, eie);
	run(0x0A4A, 0x001E, -1, 0, none);
	return 0;
}
