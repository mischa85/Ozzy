/* SPDX-License-Identifier: MIT */
/*
 * Isochronous PCM in of plain frames (ozzy_isoc_input.h).
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "ozzy_isoc_input.h"
#include "../shared/ozzy_log.h"

static const struct isoc_fb_config no_feedback = { ISOC_FB_NONE, 0, 0 };

/* IN transfer @i: its packets one after the other in its buffer, each the bytes per interval */
static bool submit_in(struct ozzy_isoc_input *in, uint32_t i)
{
	struct ozzy_isoc_input_xfer *t = &in->in[i];
	const struct ozzy_isoc_input_config *c = &in->config;
	uint32_t p;

	t->count = ozzy_isoc_input_packets(c->in_interval);
	for (p = 0; p < t->count; p++) {
		t->packets[p].offset = p * c->in_max_packet;
		t->packets[p].length = c->in_max_packet;
	}
	if (ozzy_bus_submit_isoc(in->bus, c->in_addr, c->xfer[i], c->xfer_size, t->packets, t->count,
				 (void *)(uintptr_t)i))
		return true;
	ozzy_log("isoc: input submission failed (transfer %u)", i);
	return false;
}

bool ozzy_isoc_input_start(struct ozzy_isoc_input *in, const struct ozzy_bus *bus,
			   const struct ozzy_isoc_input_config *config)
{
	uint32_t i;

	in->bus = bus;
	in->config = *config;
	if (!bus || !config->in_addr || !config->frame_bytes || !config->in_max_packet ||
	    (!config->ring && !config->deliver) || (config->ring && !config->ring_frames))
		return false;
	if (config->xfer_size < ozzy_isoc_input_xfer_bytes(config->in_max_packet, config->in_interval)) {
		ozzy_log("isoc: input transfers of %u bytes don't fit their %u-byte buffers",
			 ozzy_isoc_input_xfer_bytes(config->in_max_packet, config->in_interval), config->xfer_size);
		return false;
	}
	for (i = 0; i < ISOC_IN_TRANSFERS; i++)
		if (!config->xfer[i])
			return false;

	in->uframes = isoc_fb_interval_uframes(config->in_interval);
	isoc_fb_init(&in->nominal, config->rate, &no_feedback, 0);
	if (config->in_max_packet / config->frame_bytes < isoc_fb_max_frames(config->rate, in->uframes))
		ozzy_log("isoc: PCM in wMaxPacketSize %u may be too small for %u Hz", config->in_max_packet,
			 config->rate);
	ozzy_log("isoc in: %u microframe(s) per packet, %u packets per transfer, %u-byte frames", in->uframes,
		 ozzy_isoc_input_packets(config->in_interval), config->frame_bytes);

	if (config->ring)
		memset(config->ring, 0, config->ring_frames * config->frame_bytes);
	in->in_frame = 0;
	in->logged = false;
	in->running = true;

	for (i = 0; i < ISOC_IN_TRANSFERS; i++)
		submit_in(in, i);
	return true;
}

/* @frames frames from @src (NULL: silence) into the ring at the frame count, wrapping */
static void to_ring(struct ozzy_isoc_input *in, const uint8_t *src, uint32_t frames)
{
	const struct ozzy_isoc_input_config *c = &in->config;
	uint32_t pos = ozzy_mod_u64(in->in_frame, c->ring_frames);

	while (frames) {
		uint32_t n = c->ring_frames - pos < frames ? c->ring_frames - pos : frames;
		uint8_t *dst = c->ring + pos * c->frame_bytes;

		if (src) {
			memcpy(dst, src, n * c->frame_bytes);
			src += n * c->frame_bytes;
		} else {
			memset(dst, 0, n * c->frame_bytes);
		}
		in->in_frame += n;
		frames -= n;
		pos = 0;
	}
}

static void take(struct ozzy_isoc_input *in, const uint8_t *src, uint32_t frames)
{
	const struct ozzy_isoc_input_config *c = &in->config;

	if (!frames)
		return;
	if (c->ring) {
		to_ring(in, src, frames);
	} else {
		c->deliver(c->owner, src, frames);
		in->in_frame += frames;
	}
}

bool ozzy_isoc_input_complete(struct ozzy_isoc_input *in, uint8_t pipe, void *xfer, int status,
			      struct ozzy_isoc_packet *packets, uint32_t count)
{
	const struct ozzy_isoc_input_config *c = &in->config;
	uint32_t i = (uint32_t)(uintptr_t)xfer;
	uint32_t p;

	if (pipe != c->in_addr)
		return false;
	if (!in->running || status == OZZY_STATUS_ABORTED || i >= ISOC_IN_TRANSFERS)
		return true;

	for (p = 0; p < count; p++) {
		/* Every packet moves the nominal count on, so a failed one knows what it lost */
		uint32_t nominal = isoc_fb_out_frames(&in->nominal, in->uframes);
		uint32_t actual = packets[p].actual < packets[p].length ? packets[p].actual : packets[p].length;

		if (packets[p].status != 0 || status != 0) {
			take(in, NULL, nominal);
			continue;
		}
		if (!in->logged && actual) {
			in->logged = true;
			ozzy_log("isoc: first input packet: %u bytes (%u frames)", actual, actual / c->frame_bytes);
		}
		take(in, c->xfer[i] + packets[p].offset, actual / c->frame_bytes);
	}
	if (!c->ring && c->done)
		c->done(c->owner);
	submit_in(in, i);
	return true;
}
