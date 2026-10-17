/* SPDX-License-Identifier: MIT */
/*
 * ozzy_midi_queue_write, the MIDI out queue in front of the shared ring (OzzyMIDI, a future
 * Windows MIDI client): bytes reach the ring in order across the queue's wrap, what waits goes
 * first, and what doesn't fit the queue either is counted as dropped.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "engine/shared/ozzy_shared_data.h"
#include "check.h"
#include <string.h>

int main(void)
{
	static struct ozzy_midi_buffer ring;
	static uint8_t store[256], data[4096], out[8192];
	struct ozzy_midi_queue q = { store, sizeof(store) - 1, 0, 0, 0 };
	uint32_t i, moved, got = 0, sent = 0;

	for (i = 0; i < sizeof(data); i++)
		data[i] = (uint8_t)(i * 7 + 1);

	/* The ring takes 1023 bytes, the queue 256 more; 1500 written: 221 dropped */
	moved = ozzy_midi_queue_write(&ring, &q, data, 1500);
	CHECK(moved == OZZY_MIDI_RING_SIZE - 1, "moved %u", moved);
	CHECK(q.tail - q.head == sizeof(store) && q.dropped == 1500 - moved - sizeof(store), "queued %u, dropped %u",
	      q.tail - q.head, q.dropped);
	sent = moved + sizeof(store);

	/* Drain a bit at a time; 50 new bytes after the first drain must come out after the queued ones */
	while (got < sent + 50) {
		got += ozzy_midi_ring_pop(&ring, out + got, 100);
		ozzy_midi_queue_write(&ring, &q, got == 100 ? data + 2000 : NULL, got == 100 ? 50 : 0);
	}
	CHECK(!ozzy_midi_queue_pending(&q) && q.dropped == 1500 - (OZZY_MIDI_RING_SIZE - 1) - sizeof(store),
	      "%u bytes still queued, %u dropped", q.tail - q.head, q.dropped);
	CHECK(!memcmp(out, data, sent), "queued bytes out of order");
	CHECK(!memcmp(out + sent, data + 2000, 50), "new bytes passed the queued ones");

	/* Empty queue, room in the ring: straight through, nothing queued */
	moved = ozzy_midi_queue_write(&ring, &q, data, 10);
	CHECK(moved == 10 && !ozzy_midi_queue_pending(&q), "direct write moved %u", moved);
	return CHECK_DONE("midi queue");
}
