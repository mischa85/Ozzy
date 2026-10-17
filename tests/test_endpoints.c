/* SPDX-License-Identifier: MIT */
/*
 * ozzy_engine_bind_endpoints on the endpoint lists every platform hands it: a Xone (fixed
 * addresses, with endpoints it doesn't use around them) and the Akai EIE Pro (every pipe found
 * by type and direction, on two interfaces), plus a High Speed high-bandwidth wMaxPacketSize.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "fake_bus.h"
#include "check.h"
#include <stdlib.h>

#define N(a) (sizeof(a) / sizeof((a)[0]))

static struct ozzy_engine *engine(uint16_t vid, uint16_t pid)
{
	const struct ozzy_device *model = ozzy_device_lookup(vid, pid);

	return ozzy_engine_init(malloc(ozzy_engine_size(model)), model);
}

/* The endpoint @bound picked, as an address (0: none) */
static uint8_t addr(const struct ozzy_usb_endpoint *ep)
{
	return ep ? ep->address : 0;
}

int main(void)
{
	/* Xone:DB4 at alt 1: the isoc keep-alive and the unused bulk OUT next to its pipes */
	static const struct ozzy_usb_endpoint db4[] = {
		{ 0, 0x02, 0x01, 1, 4, NULL },
		{ 0, 0x83, 0x03, 1, 64, NULL },
		{ 0, 0x04, 0x02, 0, 512, NULL },
		{ 0, 0x05, 0x03, 1, 964, NULL },
		{ 1, 0x81, 0x01, 1, 4, NULL },
		{ 1, 0x86, 0x03, 1, 1024, NULL },
	};
	/* Akai EIE Pro at alt 1 (.claude/re/akai_eie_pro.md); 0x02 high bandwidth here */
	static const struct ozzy_usb_endpoint eie[] = {
		{ 0, 0x02, 0x05, 1, 0x0800 | 312, NULL },
		{ 0, 0x83, 0x02, 0, 512, NULL },
		{ 0, 0x04, 0x02, 0, 512, NULL },
		{ 1, 0x81, 0x05, 1, 3, NULL },
		{ 1, 0x86, 0x02, 0, 512, NULL },
	};
	/* The PCM IN endpoint missing (wrong interface) */
	static const struct ozzy_usb_endpoint db4_partial[] = {
		{ 0, 0x05, 0x03, 1, 964, NULL },
		{ 0, 0x83, 0x03, 1, 64, NULL },
		{ 0, 0x86, 0x03, 1, 1024, NULL },
	};
	const struct ozzy_usb_endpoint *bound[OZZY_PIPE_ROLES];
	struct ozzy_engine *e;

	e = engine(0x0A4A, 0xFFDB);
	ozzy_engine_bind_endpoints(e, db4, N(db4), bound);
	CHECK(addr(bound[OZZY_PIPE_PCM_OUT]) == 0x05, "DB4 PCM out 0x%02x", addr(bound[OZZY_PIPE_PCM_OUT]));
	CHECK(addr(bound[OZZY_PIPE_PCM_IN]) == 0x86, "DB4 PCM in 0x%02x", addr(bound[OZZY_PIPE_PCM_IN]));
	CHECK(addr(bound[OZZY_PIPE_MIDI_IN]) == 0x83, "DB4 MIDI in 0x%02x", addr(bound[OZZY_PIPE_MIDI_IN]));
	CHECK(!bound[OZZY_PIPE_MIDI_OUT] && !bound[OZZY_PIPE_FEEDBACK], "DB4 has no MIDI out or feedback pipe");
	CHECK(e->pipe[OZZY_PIPE_PCM_OUT].max_packet_size == 964 && e->pipe[OZZY_PIPE_PCM_OUT].transfer_type == 3,
	      "DB4 PCM out %u type %u", e->pipe[OZZY_PIPE_PCM_OUT].max_packet_size,
	      e->pipe[OZZY_PIPE_PCM_OUT].transfer_type);
	free(e);

	e = engine(0x0A4A, 0xFFDB);
	ozzy_engine_bind_endpoints(e, db4_partial, N(db4_partial), bound);
	CHECK(addr(bound[OZZY_PIPE_PCM_OUT]) == 0x05 && !bound[OZZY_PIPE_PCM_IN],
	      "PCM in on the wrong interface must not bind (0x%02x)", addr(bound[OZZY_PIPE_PCM_IN]));
	CHECK(e->pipe[OZZY_PIPE_PCM_IN].address == 0, "unbound role told to the engine");
	free(e);

	e = engine(0x09E8, 0x0010);
	ozzy_engine_bind_endpoints(e, eie, N(eie), bound);
	CHECK(addr(bound[OZZY_PIPE_PCM_OUT]) == 0x02, "EIE PCM out 0x%02x", addr(bound[OZZY_PIPE_PCM_OUT]));
	CHECK(addr(bound[OZZY_PIPE_MIDI_OUT]) == 0x04, "EIE MIDI out 0x%02x", addr(bound[OZZY_PIPE_MIDI_OUT]));
	CHECK(addr(bound[OZZY_PIPE_MIDI_IN]) == 0x83, "EIE MIDI in 0x%02x", addr(bound[OZZY_PIPE_MIDI_IN]));
	CHECK(addr(bound[OZZY_PIPE_PCM_IN]) == 0x86, "EIE PCM in 0x%02x", addr(bound[OZZY_PIPE_PCM_IN]));
	CHECK(addr(bound[OZZY_PIPE_FEEDBACK]) == 0x81, "EIE feedback 0x%02x", addr(bound[OZZY_PIPE_FEEDBACK]));
	CHECK(e->pipe[OZZY_PIPE_PCM_OUT].max_packet_size == 624 && e->pipe[OZZY_PIPE_PCM_OUT].transfer_type == 1,
	      "EIE PCM out %u bytes per interval, type %u", e->pipe[OZZY_PIPE_PCM_OUT].max_packet_size,
	      e->pipe[OZZY_PIPE_PCM_OUT].transfer_type);
	free(e);

	return CHECK_DONE("endpoints");
}
