/* SPDX-License-Identifier: MIT */
/*
 * Ploytec bulk in / isochronous out devices (ploytec_isoc_engine.h).
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "ploytec_isoc_engine.h"
#include "ploytec_io.h"
#include "../../../common/devices/ploytec/ploytec_handshake.h"
#include "../../shared/ozzy_log.h"

#define IN_QUEUE_DEFAULT   1   /* OZZY_SETTING_PCM_IN_QUEUE: 2 transfers in flight, as before the setting */

/* Feedback format (ploytec_defs.h) */
static const struct isoc_fb_config feedback = PLOYTEC_ISOC_FEEDBACK;

static struct ploytec_isoc_engine *to_isoc(struct ozzy_engine *e)
{
	return (struct ploytec_isoc_engine *)e;
}

static void isoc_set_endpoint(struct ozzy_engine *e, enum ozzy_pipe_role role, uint8_t address,
			      uint8_t transfer_type, uint16_t max_packet_size, uint8_t interval)
{
	struct ploytec_isoc_engine *pe = to_isoc(e);

	switch (role) {
	case OZZY_PIPE_PCM_OUT:
		pe->out_addr = address;
		pe->out_max_packet = max_packet_size;
		pe->out_interval = interval;
		break;
	case OZZY_PIPE_PCM_IN:   pe->in_addr = address; break;
	case OZZY_PIPE_MIDI_IN:  pe->midi_in_addr = address; break;
	case OZZY_PIPE_MIDI_OUT: pe->midi_out_addr = address; break;
	case OZZY_PIPE_FEEDBACK:
		pe->fb_addr = address;
		pe->fb_max_packet = max_packet_size;
		pe->fb_interval = interval;
		break;
	default:
		break;
	}
}

/* MIDI IN reads of the framing's transfer size (AJ::configurationDone), one slot each */
static void submit_midi_in(struct ploytec_isoc_engine *pe, uint32_t slot)
{
	if (!ploytec_submit_midi_in(&pe->base, pe->midi_in_addr, &pe->midi, slot))
		ploytec_log("isoc: MIDI input submission failed (slot %u)", slot);
}

/*
 * fill_out - The plain frames of one OUT transfer from the attached ring, with the output
 * routing; silence on a platform with rings and none attached (Linux: ozzy_pcm_fill_iso_out).
 */
static void fill_out(void *owner, uint8_t *dst_ring, uint32_t dst_frames, uint64_t first, uint32_t frames)
{
	struct ozzy_engine *e = (struct ozzy_engine *)owner;
	const struct ozzy_pcm_ring *ring = ozzy_engine_take_pcm_ring(e, OZZY_PCM_OUT);
	uint32_t frame_bytes = e->model->out_channels * 3;
	uint32_t d = ozzy_mod_u64(first, dst_frames), r, f;

	if (!ring) {
		if (ozzy_engine_ring_platform(e))
			for (f = 0; f < frames; f++, d = d + 1 == dst_frames ? 0 : d + 1)
				memset(dst_ring + d * frame_bytes, 0, frame_bytes);
		return;
	}
	r = ozzy_mod_u64(e->pcm_pos[OZZY_PCM_OUT], ring->frames);
	for (f = 0; f < frames; f++) {
		if (e->out_route.identity)
			memcpy(dst_ring + d * frame_bytes, ring->base + r * frame_bytes, frame_bytes);
		else
			ozzy_route_frame(dst_ring + d * frame_bytes, ring->base + r * frame_bytes, &e->out_route, 2 * 3);
		if (++r == ring->frames)
			r = 0;
		if (++d == dst_frames)
			d = 0;
	}
	e->pcm_pos[OZZY_PCM_OUT] += frames;
	ozzy_bus_pcm_progress(e->bus, OZZY_PCM_OUT, e->pcm_pos[OZZY_PCM_OUT]);
}

static bool isoc_start(struct ozzy_engine *e)
{
	struct ploytec_isoc_engine *pe = to_isoc(e);
	volatile struct ozzy_shared_memory *shm = e->shm;
	const struct ozzy_device *model = e->model;
	uint32_t frame_bytes = model->out_channels * 3;
	struct ploytec_rate_endpoints eps;
	struct ploytec_handshake_info hs;
	struct ozzy_isoc_output_config out;
	uint32_t in_places, i;

	if (!shm)
		return false;
	if (!pe->out_addr || !pe->in_addr) {
		ploytec_log("isoc: PCM %s endpoint not found", pe->out_addr ? "in" : "out");
		return false;
	}

	/* 0. The in transfer size for this run (a change takes a restart), laid out in its ring */
	pe->in_frames = ozzy_pcm_frames(true, e->settings.value[OZZY_SETTING_PCM_IN_FRAMES]);
	pe->in_queue = ozzy_pcm_queue(e->settings.value[OZZY_SETTING_PCM_IN_QUEUE]);
	e->settings.value[OZZY_SETTING_PCM_IN_QUEUE] = (uint8_t)(pe->in_queue - 1);   /* one runs as two */
	in_places = ozzy_engine_pcm_layout(e, OZZY_PCM_IN, ploytec_in_xfer_bytes(pe->in_frames));
	if (!in_places) {
		ploytec_log("isoc: PCM in transfers of %u frames don't fit", pe->in_frames);
		return false;
	}

	/* 1. What the HAL needs: a plain frame ring for output, Ploytec sub-packets for input */
	shm->audio.device_flags = OZZY_DEVICE_FLAG_PLAIN_OUTPUT;
	shm->audio.output_ring_frames = OZZY_PLAIN_RING_FRAMES;
	shm->audio.input_ring_frames = in_places * pe->in_frames;
	shm->audio.timestamp_frames = e->anchors ? ozzy_anchor_frames(e->settings.value[OZZY_SETTING_IO_BUFFER_MAX], 1) : 0;
	shm->audio.output_bytes_per_frame = frame_bytes;
	shm->audio.input_bytes_per_frame = PLOYTEC_IN_FRAME_SIZE;
	shm->audio.input_channels = model->in_channels;
	shm->audio.output_channels = model->out_channels;
	ploytec_log("model: %s, %u in / %u out", model->name, model->in_channels, model->out_channels);

	/* 2. Handshake with the settings, with the rate requests on the PCM endpoints found;
	 * locked to S/PDIF it sets the lock rate */
	eps.pcm_in = pe->in_addr;
	eps.pcm_out = pe->out_addr;
	memset(&hs, 0, sizeof(hs));
	if (ploytec_handshake_eps(&ploytec_bus_host_ops, (void *)e->bus, ozzy_engine_start_rate(e, pe->default_rate), &eps, model->quirks,
				  &e->settings, &hs) < 0)
		return false;
	if (!ozzy_device_has_rate(model, hs.rate)) {
		/* Locked to an S/PDIF rate the device doesn't list (Linux: ploytec_adopt_rate) */
		ploytec_log("device runs at %u Hz, which it doesn't list", hs.rate);
		return false;
	}
	pe->sample_rate = hs.rate;
	shm->audio.sample_rate = pe->sample_rate;
	shm->audio.safety_offset_frames = ozzy_isoc_output_safety_offset(pe->sample_rate);
	/* Input: the transfers queued (it shrinks with them; before the setting it was the output's
	 * offset, 135-147 frames, now 160 at the defaults), at least the jitter floor */
	shm->audio.input_safety_offset_frames = ozzy_engine_safety_offset(pe->in_queue * pe->in_frames, pe->sample_rate);
	ozzy_engine_publish_settings(e, &hs.clock);
	ploytec_report(e, &hs);
	e->report.output = OZZY_INFO_OUTPUT_ISOC;
	e->report.feedback = !pe->fb_addr || feedback.format == ISOC_FB_NONE ? OZZY_INFO_FEEDBACK_NONE :
			     feedback.format == ISOC_FB_COUNT ? OZZY_INFO_FEEDBACK_COUNT : OZZY_INFO_FEEDBACK_Q16_16;
	e->report.in_flight[OZZY_PIPE_PCM_OUT] = ISOC_OUT_TRANSFERS;
	e->report.in_flight[OZZY_PIPE_PCM_IN] = (uint8_t)pe->in_queue;
	e->report.in_flight[OZZY_PIPE_FEEDBACK] = ISOC_FB_TRANSFERS;
	e->report.in_flight[OZZY_PIPE_MIDI_IN] = PLOYTEC_MIDI_IN_XFERS;
	e->report.in_flight[OZZY_PIPE_MIDI_OUT] = 1;
	/* MIDI framing on its own endpoints, as the device's kInACR picks it (ploytec_midi.h) */
	ploytec_midi_init_xfer(&pe->midi, pe->sample_rate, (model->quirks & OZZY_QUIRK_MIDI_NS7) ?
			       PLOYTEC_MIDI_XFER_NS7 : PLOYTEC_MIDI_XFER_M4_C1);

	ozzy_store_release_bool(&shm->audio.hardware_present, true);
	shm->audio.timestamp.sample_time = 0;
	ozzy_store_release_bool(&pe->midi_out_busy, false);
	pe->running = true;

	/* 3. Transfers: PCM in, PCM out with feedback, MIDI in; MIDI out from the timer */
	for (i = 0; i < pe->in_queue; i++)
		if (!ploytec_submit_pcm_in(e, pe->in_addr, pe->in_frames, i))
			ploytec_log("isoc: input submission failed (idx: %u)", i);

	memset(&out, 0, sizeof(out));
	out.rate = pe->sample_rate;
	out.frame_bytes = frame_bytes;
	out.ring_frames = OZZY_PLAIN_RING_FRAMES;   /* the HAL's ring size */
	out.anchor_frames = shm->audio.timestamp_frames;   /* 0: none */
	out.out_addr = pe->out_addr;
	out.out_max_packet = pe->out_max_packet;
	out.out_interval = pe->out_interval;
	out.fb_addr = pe->fb_addr;
	out.fb_max_packet = pe->fb_max_packet;
	out.fb_interval = pe->fb_interval;
	out.feedback = feedback;
	out.fill = fill_out;
	out.owner = e;
	/* The platform's per-transfer buffers, or the shared memory's frame ring (the HAL's) */
	if (e->mem.isoc_out[0]) {
		memcpy(out.xfer, e->mem.isoc_out, sizeof(out.xfer));
		out.xfer_size = e->mem.isoc_out_size;
	} else {
		out.ring = (uint8_t *)shm->audio.output_buffer;
		if (out.ring_frames * frame_bytes > sizeof(shm->audio.output_buffer))
			return false;
	}
	out.fb_buf = e->mem.isoc_in;
	out.fb_size = e->mem.isoc_in_size;
	pe->decode = ploytec_frame_decoder(model->in_channels);
	if (!ozzy_isoc_output_start(&pe->output, e->bus, shm, &out))
		return false;

	for (i = 0; pe->midi_in_addr && i < PLOYTEC_MIDI_IN_XFERS; i++)
		submit_midi_in(pe, i);
	if (pe->midi_out_addr)
		ozzy_bus_start_timer(e->bus, PLOYTEC_MIDI_XFER_INTERVAL_US);

	ozzy_store_release_bool(&shm->audio.driver_ready, true);
	return true;
}

/* No more resubmits, no more MIDI out timer */
static void isoc_halt(struct ozzy_engine *e)
{
	struct ploytec_isoc_engine *pe = to_isoc(e);

	pe->running = false;
	ozzy_isoc_output_stop(&pe->output);
	if (e->bus)
		ozzy_bus_stop_timer(e->bus);
}

static void isoc_suspend(struct ozzy_engine *e)
{
	isoc_halt(e);
	ploytec_log("isoc engine suspended");
}

static void isoc_stop(struct ozzy_engine *e)
{
	volatile struct ozzy_shared_memory *shm = e->shm;

	isoc_halt(e);
	if (!shm)
		return;
	shm->audio.driver_ready = false;
	shm->audio.hardware_present = false;
	ploytec_log("isoc engine stopped");
}

static void isoc_isoc_complete(struct ozzy_engine *e, uint8_t pipe, void *xfer, int status,
			       struct ozzy_isoc_packet *packets, uint32_t count)
{
	struct ploytec_isoc_engine *pe = to_isoc(e);

	if (e->bus && pe->running)
		ozzy_isoc_output_complete(&pe->output, pipe, xfer, status, packets, count);
}

static void isoc_complete(struct ozzy_engine *e, uint8_t pipe, void *xfer, int status, uint32_t bytes)
{
	struct ploytec_isoc_engine *pe = to_isoc(e);
	volatile struct ozzy_shared_memory *shm = e->shm;
	uint32_t idx = (uint32_t)(uintptr_t)xfer;

	if (!e->bus || !pe->running || status == OZZY_STATUS_ABORTED)
		return;

	if (pipe == pe->in_addr) {
		uint32_t next = ploytec_pcm_next(idx, pe->in_queue, e->mem.pcm_in_slots);

		if (status == 0 && bytes != ploytec_in_xfer_bytes(pe->in_frames))
			ozzy_engine_stats_short(e, OZZY_PCM_IN);
		ploytec_decode_pcm_in(e, pe->decode, pe->in_frames, idx, status == 0 ? bytes : 0);
		if (!ploytec_submit_pcm_in(e, pe->in_addr, pe->in_frames, next))
			ploytec_log("isoc: input submission failed (idx: %u)", next);
	} else if (pipe == pe->midi_in_addr) {
		if (status == 0)
			ploytec_midi_in_to_ring(&pe->midi, shm, ploytec_midi_in_slot(e, idx),
						bytes < PLOYTEC_MIDI_IN_SLOT ? bytes : PLOYTEC_MIDI_IN_SLOT);
		submit_midi_in(pe, idx);
	} else if (pipe == pe->midi_out_addr) {
		if (status != 0)
			ploytec_log("isoc: MIDI output error 0x%x", status);
		ozzy_store_release_bool(&pe->midi_out_busy, false);
	}
}

/*
 * isoc_timer - Every millisecond (bus timer): the next MIDI OUT transfer if the previous one
 * is done. Like the official driver it also sends idle transfers. Same pacing as Linux
 * (ozzy_midi_out_tick).
 */
static void isoc_timer(struct ozzy_engine *e)
{
	struct ploytec_isoc_engine *pe = to_isoc(e);
	volatile struct ozzy_shared_memory *shm = e->shm;
	uint8_t *buf;
	uint32_t n;

	if (!pe->running || !shm || !pe->midi_out_addr)
		return;
	if (ozzy_xchg_acquire_bool(&pe->midi_out_busy, true))
		return;

	buf = e->mem.midi_out;
	n = ploytec_midi_xfer_begin(&pe->midi, buf);
	ozzy_midi_ring_pop(&shm->midi_out, buf, n);

	if (!ozzy_bus_submit(e->bus, pe->midi_out_addr, buf, pe->midi.xfer_size, NULL)) {
		ploytec_log("isoc: MIDI output submission failed");
		ozzy_store_release_bool(&pe->midi_out_busy, false);
	}
}

/* Isochronous OUT moves a transfer's microframes of frames, the most at the highest rate */
static uint32_t isoc_transfer_frames(const struct ozzy_engine *e, enum ozzy_pcm_dir dir)
{
	uint32_t rates[OZZY_MAX_RATES];

	if (dir == OZZY_PCM_IN)
		return ((const struct ploytec_isoc_engine *)e)->in_frames;
	return rates[ozzy_device_rates(e->model, rates) - 1] * ISOC_TRANSFER_UFRAMES / ISOC_FB_UFRAMES_PER_SEC;
}

/* PCM in is Ploytec's 8-frame sub-packets: its transfer size and queue are settings; out is
 * isochronous */
static uint32_t isoc_setting_quirks(const struct ozzy_engine *e)
{
	return e->model->quirks | OZZY_QUIRK_PCM_IN_FRAMES | OZZY_QUIRK_PCM_IN_QUEUE |
	       (e->anchors ? OZZY_QUIRK_CLOCK_ANCHORS : 0);
}

/* The in queue starts at 2 here (the setting's own default is the Xones' 4) */
static int isoc_setting_default(const struct ozzy_engine *e, unsigned int id)
{
	(void)e;
	return id == OZZY_SETTING_PCM_IN_QUEUE ? IN_QUEUE_DEFAULT : -1;
}

static const struct ozzy_engine_ops ploytec_isoc_engine_ops = {
	isoc_set_endpoint,
	isoc_start,
	isoc_stop,
	isoc_suspend,
	isoc_complete,
	isoc_isoc_complete,
	NULL,                   /* midi_out_pending: MIDI goes out on the timer */
	isoc_timer,
	isoc_setting_quirks,
	isoc_transfer_frames,
	isoc_setting_default,
};

/* Channels and rates per device (common/ozzy_devices.def); run at the highest rate. Endpoints
 * found by type, as both platforms describe them (ploytec_defs.h). */
void ploytec_isoc_engine_init(struct ploytec_isoc_engine *pe, const struct ozzy_device *model)
{
	static const struct ozzy_ep pcm_out = PLOYTEC_ISOC_EP_PCM_OUT;
	static const struct ozzy_ep midi_out = PLOYTEC_ISOC_EP_MIDI_OUT;
	static const struct ozzy_ep midi_in = PLOYTEC_ISOC_EP_MIDI_IN;
	static const struct ozzy_ep pcm_in = PLOYTEC_ISOC_EP_PCM_IN;
	static const struct ozzy_ep fb = PLOYTEC_ISOC_EP_FEEDBACK;
	struct ozzy_device_profile *p = &pe->base.profile;
	uint32_t rates[OZZY_MAX_RATES];

	pe->base.ops = &ploytec_isoc_engine_ops;
	pe->base.model = model;
	pe->default_rate = rates[ozzy_device_rates(model, rates) - 1];
	pe->sample_rate = pe->default_rate;
	pe->in_frames = ozzy_pcm_frames(true, ozzy_engine_setting_default(&pe->base, OZZY_SETTING_PCM_IN_FRAMES));
	pe->in_queue = ozzy_pcm_queue(IN_QUEUE_DEFAULT);
	pe->out_interval = 1;

	p->interface_count = PLOYTEC_NUM_INTERFACES;
	p->interfaces[0] = 0;
	p->interfaces[1] = 1;
	p->alt_setting = PLOYTEC_ALT_SETTING;
	p->pipes[OZZY_PIPE_PCM_OUT] = pcm_out;
	p->pipes[OZZY_PIPE_MIDI_OUT] = midi_out;
	p->pipes[OZZY_PIPE_MIDI_IN] = midi_in;
	p->pipes[OZZY_PIPE_PCM_IN] = pcm_in;
	p->pipes[OZZY_PIPE_FEEDBACK] = fb;
}
