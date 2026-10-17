/* SPDX-License-Identifier: MIT */
/*
 * Isochronous PCM out paced by a feedback endpoint (ozzy_isoc_output.h).
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "ozzy_isoc_output.h"
#include "../shared/ozzy_log.h"
#include <string.h>

static const struct isoc_fb_config no_feedback = { ISOC_FB_NONE, 0, 0 };

/*
 * submit_out - OUT transfer @i: the frames the device consumed in each packet's interval
 * (isoc_fb_layout, shared with Linux), pointed at in the frame ring.
 */
static bool submit_out(struct ozzy_isoc_output *out, uint32_t i)
{
	struct ozzy_isoc_output_xfer *t = &out->out[i];
	const struct ozzy_isoc_output_config *c = &out->config;
	uint32_t p;

	uint64_t first = out->out_frame;

	t->count = isoc_fb_packets(out->out_uframes);
	isoc_fb_layout(&out->fb, t->count, out->out_uframes, out->out_max_frames, t->frames);
	for (p = 0; p < t->count; p++) {
		t->packets[p].offset = (uint32_t)(out->out_frame % c->ring_frames) * c->frame_bytes;
		t->packets[p].length = t->frames[p] * c->frame_bytes;
		out->out_frame += t->frames[p];
	}
	if (c->fill)
		c->fill(c->owner, (uint8_t *)out->shm->audio.output_buffer, c->ring_frames, first,
			(uint32_t)(out->out_frame - first));
	if (ozzy_bus_submit_isoc(out->bus, c->out_addr, (void *)out->shm->audio.output_buffer,
				 c->ring_frames * c->frame_bytes, t->packets, t->count, (void *)(uintptr_t)i))
		return true;
	ozzy_log("isoc: output submission failed (transfer %u)", i);
	return false;
}

static bool submit_feedback(struct ozzy_isoc_output *out, uint32_t i)
{
	struct ozzy_isoc_output_xfer *t = &out->fb_xfer[i];
	const struct ozzy_isoc_output_config *c = &out->config;
	uint32_t base = i * ISOC_TRANSFER_UFRAMES * c->fb_max_packet;
	uint32_t p;

	t->count = isoc_fb_packets(out->fb.uframes);
	for (p = 0; p < t->count; p++) {
		t->packets[p].offset = base + p * c->fb_max_packet;
		t->packets[p].length = c->fb_max_packet;
	}
	if (ozzy_bus_submit_isoc(out->bus, c->fb_addr, (void *)out->shm->isoc_in_usb_buffer,
				 sizeof(out->shm->isoc_in_usb_buffer), t->packets, t->count, (void *)(uintptr_t)i))
		return true;
	ozzy_log("isoc: feedback submission failed (transfer %u)", i);
	return false;
}

bool ozzy_isoc_output_start(struct ozzy_isoc_output *out, const struct ozzy_bus *bus,
			    volatile struct ozzy_shared_memory *shm, const struct ozzy_isoc_output_config *config)
{
	uint32_t ring_bytes = config->ring_frames * config->frame_bytes;
	bool fb_fits;
	uint32_t i;

	out->bus = bus;
	out->shm = shm;
	out->config = *config;
	if (!bus || !shm || !config->out_addr || ring_bytes > sizeof(shm->audio.output_buffer))
		return false;

	fb_fits = config->fb_addr &&
		  ISOC_FB_TRANSFERS * ISOC_TRANSFER_UFRAMES * config->fb_max_packet <= sizeof(shm->isoc_in_usb_buffer);
	if (config->fb_addr && !fb_fits)
		ozzy_log("isoc: feedback wMaxPacketSize %u too large", config->fb_max_packet);
	if (!isoc_fb_init(&out->fb, config->rate, fb_fits ? &config->feedback : &no_feedback,
			  fb_fits ? config->fb_interval : 0))
		ozzy_log("isoc: feedback bInterval %u not supported, PCM out runs at the nominal rate",
			 config->fb_interval);

	out->out_uframes = isoc_fb_interval_uframes(config->out_interval);
	out->out_max_frames = config->out_max_packet / config->frame_bytes;
	if (out->out_max_frames < isoc_fb_max_frames(config->rate, out->out_uframes))
		ozzy_log("isoc: PCM out wMaxPacketSize %u is too small for %u Hz", config->out_max_packet, config->rate);
	ozzy_log("isoc out: %u microframe(s) per packet, up to %u frames, feedback every %u microframe(s)",
		 out->out_uframes, out->out_max_frames, out->fb.uframes);

	/* Silence in the ring, clock at 0 */
	memset((void *)shm->audio.output_buffer, 0, ring_bytes);
	out->out_frame = 0;
	out->played_frames = 0;
	out->next_anchor = config->anchor_frames;
	out->fb_logged = false;
	out->running = true;

	for (i = 0; fb_fits && i < ISOC_FB_TRANSFERS; i++)
		submit_feedback(out, i);
	for (i = 0; i < ISOC_OUT_TRANSFERS; i++)
		submit_out(out, i);
	return true;
}

/*
 * publish_timestamps - Advance the device's sample time by the frames this transfer played,
 * and publish an anchor (seqlock, reader: OzzyHAL::GetZeroTimeStamp) in the microframe that
 * reaches each multiple of anchor_frames, at that microframe's completion time.
 */
static void publish_timestamps(struct ozzy_isoc_output *out, const struct ozzy_isoc_output_xfer *t)
{
	uint32_t p;

	for (p = 0; p < t->count; p++) {
		volatile struct ozzy_timestamp *ts;
		uint32_t seq;

		out->played_frames += t->frames[p];
		if (out->played_frames < out->next_anchor)
			continue;

		ts = &out->shm->audio.timestamp;
		seq = ts->sequence;
		ozzy_store_relaxed_u32(&ts->sequence, seq + 1);
		ozzy_fence_release();
		ozzy_store_relaxed_u64(&ts->sample_time, out->next_anchor);
		ozzy_store_relaxed_u64(&ts->host_time, t->packets[p].time ? t->packets[p].time : ozzy_bus_time(out->bus));
		ozzy_store_release_u32(&ts->sequence, seq + 2);
		out->next_anchor += out->config.anchor_frames;
	}
}

bool ozzy_isoc_output_complete(struct ozzy_isoc_output *out, uint8_t pipe, void *xfer, int status,
			       struct ozzy_isoc_packet *packets, uint32_t count)
{
	uint32_t i = (uint32_t)(uintptr_t)xfer;
	uint32_t p;

	if (pipe == out->config.out_addr) {
		if (!out->running || status == OZZY_STATUS_ABORTED)
			return true;
		publish_timestamps(out, &out->out[i]);
		submit_out(out, i);
		return true;
	}
	if (pipe == out->config.fb_addr && out->config.fb_addr) {
		if (!out->running || status == OZZY_STATUS_ABORTED)
			return true;
		for (p = 0; p < count; p++) {
			const uint8_t *pkt = (const uint8_t *)out->shm->isoc_in_usb_buffer + packets[p].offset;
			uint32_t len = packets[p].status == 0 ? packets[p].actual : 0;

			if (!out->fb_logged) {
				out->fb_logged = true;
				ozzy_log("isoc: first feedback packet: %u bytes %02x %02x %02x", len, pkt[0], pkt[1], pkt[2]);
			}
			if (isoc_fb_packet(&out->fb, pkt, len))
				ozzy_log("isoc: more than %u feedback packets were not %u bytes long, PCM out runs at the nominal rate",
					 out->fb.cfg.max_errors, out->fb.cfg.packet_size);
		}
		submit_feedback(out, i);
		return true;
	}
	return false;
}
