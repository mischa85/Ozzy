/* SPDX-License-Identifier: MIT */
/*
 * A fake USB bus for host tests of engine/: transfers queue up and complete when the test
 * says so, control requests answer like a Ploytec device (the rate read returns the rate last
 * set), and everything the engine does can be traced, one line per call.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef FAKE_BUS_H
#define FAKE_BUS_H

#include "engine/core/ozzy_engine.h"
#include <stdio.h>

struct fake_xfer {
	uint8_t pipe;
	uint8_t *buf;
	uint32_t size;
	void *xfer;
	bool isoc;
	struct ozzy_isoc_packet *packets;
	uint32_t count;
};

struct fake_bus {
	struct ozzy_bus bus;
	struct ozzy_shared_memory *shm;    /* offsets in the trace are relative to it */
	FILE *trace;                       /* NULL: no trace */
	struct fake_xfer queue[512];
	unsigned int head, tail;
	uint8_t rate[3];                   /* last UAC1 SET_CUR rate */
	uint64_t ticks;
	bool timer_on;
	uint64_t progress[OZZY_PCM_DIRS];  /* last pcm_progress */
	bool rings;                        /* has pcm_progress (a ring platform) */
	struct ozzy_engine *engine;        /* of the last completion: abort completes to it */
};

/* A bus on @shm; @rings: a platform that attaches PCM rings */
void fake_bus_init(struct fake_bus *fb, struct ozzy_shared_memory *shm, bool rings, FILE *trace);
bool fake_bus_pop(struct fake_bus *fb, struct fake_xfer *x);
void fake_bus_push(struct fake_bus *fb, const struct fake_xfer *x);

/* Complete @x: IN transfers get @fill bytes first (NULL: leave the buffer), isochronous
 * packets complete in full */
void fake_bus_complete(struct fake_bus *fb, struct ozzy_engine *e, struct fake_xfer *x,
		       void (*fill)(struct fake_xfer *x, void *arg), void *arg);

/* A small deterministic generator (the same on every host) */
static inline uint32_t fake_rand(uint32_t *state)
{
	*state = *state * 1664525u + 1013904223u;
	return *state >> 8;
}

#endif /* FAKE_BUS_H */
