/* SPDX-License-Identifier: MIT */
/*
 * Ploytec isochronous in and out, the Tascam US-366 (ploytec_us3xx_engine.h).
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "ploytec_us3xx_engine.h"
#include "ploytec_io.h"
#include "../../../common/devices/ploytec/ploytec_handshake.h"
#include "../../shared/ozzy_log.h"

/* MIDI IN reads: half the MIDI IN memory each (DMA-aligned); the device sent 8 or 20 bytes */
#define MIDI_READ_SIZE   (OZZY_MIDI_IN_SIZE / PLOYTEC_US3XX_MIDI_READS)
/* A read slot stops after this many errors in a row instead of resubmitting forever */
#define MAX_READ_ERRORS  8
#define CABLE            0

static const struct isoc_fb_config no_feedback = { ISOC_FB_NONE, 0, 0 };

static struct ploytec_us3xx_engine *to_us3xx(struct ozzy_engine *e)
{
	return (struct ploytec_us3xx_engine *)e;
}

static void us3xx_set_endpoint(struct ozzy_engine *e, enum ozzy_pipe_role role, uint8_t address,
			       uint8_t transfer_type, uint16_t max_packet_size, uint8_t interval)
{
	struct ploytec_us3xx_engine *ue = to_us3xx(e);

	switch (role) {
	case OZZY_PIPE_PCM_OUT:
		ue->out_addr = address;
		ue->out_max_packet = max_packet_size;
		ue->out_interval = interval;
		break;
	case OZZY_PIPE_PCM_IN:
		ue->in_addr = address;
		ue->in_max_packet = max_packet_size;
		ue->in_interval = interval;
		break;
	case OZZY_PIPE_MIDI_IN:
		ue->midi_in_addr = address;
		break;
	case OZZY_PIPE_MIDI_OUT:
		ue->midi_out_addr = address;
		ue->midi_out_max_packet = max_packet_size;
		break;
	default:
		break;
	}
}

/* --- PCM --- */

static void fill_out(void *owner, uint8_t *dst_ring, uint32_t dst_frames, uint64_t first, uint32_t frames)
{
	ozzy_engine_plain_out((struct ozzy_engine *)owner, dst_ring, dst_frames, first, frames);
}

static void deliver_in(void *owner, const uint8_t *src, uint32_t frames)
{
	ozzy_engine_plain_in((struct ozzy_engine *)owner, src, frames);
}

static void delivered_in(void *owner)
{
	ozzy_engine_plain_in_done((struct ozzy_engine *)owner);
}

/* PCM out with ozzy_isoc_output at the nominal rate: the platform's buffers or the HAL's ring */
static bool start_output(struct ploytec_us3xx_engine *ue)
{
	struct ozzy_engine *e = &ue->base;
	volatile struct ozzy_shared_memory *shm = e->shm;
	uint32_t frame_bytes = e->model->out_channels * 3;
	struct ozzy_isoc_output_config out;

	memset(&out, 0, sizeof(out));
	out.rate = ue->sample_rate;
	out.frame_bytes = frame_bytes;
	out.ring_frames = OZZY_PLAIN_RING_FRAMES;
	out.anchor_frames = shm->audio.timestamp_frames;   /* 0: none */
	out.out_addr = ue->out_addr;
	out.out_max_packet = ue->out_max_packet;
	out.out_interval = ue->out_interval;
	out.feedback = no_feedback;
	out.fill = fill_out;
	out.owner = e;
	if (e->mem.isoc_out[0]) {
		memcpy(out.xfer, e->mem.isoc_out, sizeof(out.xfer));
		out.xfer_size = e->mem.isoc_out_size;
	} else {
		out.ring = (uint8_t *)shm->audio.output_buffer;
		if (out.ring_frames * frame_bytes > sizeof(shm->audio.output_buffer))
			return false;
	}
	return ozzy_isoc_output_start(&ue->output, e->bus, shm, &out);
}

/*
 * PCM in with ozzy_isoc_input. Its transfers go to the platform's PCM in buffers, or in the shared
 * memory's input buffer after the HAL's plain ring. Received frames go to the attached ring on a
 * ring platform, else to the HAL's ring.
 */
static bool start_input(struct ploytec_us3xx_engine *ue)
{
	struct ozzy_engine *e = &ue->base;
	volatile struct ozzy_shared_memory *shm = e->shm;
	struct ozzy_engine_memory *m = &e->mem;
	uint32_t frame_bytes = e->model->in_channels * 3;
	uint32_t ring_bytes = OZZY_DMA_ROUND(OZZY_PLAIN_RING_FRAMES * frame_bytes);
	uint32_t xfer_bytes = OZZY_DMA_ROUND(ozzy_isoc_input_xfer_bytes(ue->in_max_packet, ue->in_interval));
	struct ozzy_isoc_input_config in;
	uint32_t i;

	memset(&in, 0, sizeof(in));
	in.rate = ue->sample_rate;
	in.frame_bytes = frame_bytes;
	in.in_addr = ue->in_addr;
	in.in_max_packet = ue->in_max_packet;
	in.in_interval = ue->in_interval;
	if (m->pcm_in_ring) {
		if (ring_bytes + ISOC_IN_TRANSFERS * xfer_bytes > m->pcm_in_ring_bytes) {
			ploytec_log("us3xx: PCM in transfers of %u bytes don't fit", xfer_bytes);
			return false;
		}
		for (i = 0; i < ISOC_IN_TRANSFERS; i++)
			in.xfer[i] = m->pcm_in_ring + ring_bytes + i * xfer_bytes;
		in.xfer_size = xfer_bytes;
	} else {
		if (ISOC_IN_TRANSFERS > OZZY_PCM_OWN_SLOTS)
			return false;
		for (i = 0; i < ISOC_IN_TRANSFERS; i++)
			in.xfer[i] = m->pcm_in[i];
		in.xfer_size = OZZY_PCM_SLOT_MIN;
	}
	if (ozzy_engine_ring_platform(e)) {
		in.deliver = deliver_in;
		in.done = delivered_in;
		in.owner = e;
	} else {
		in.ring = (uint8_t *)shm->audio.input_buffer;
		in.ring_frames = OZZY_PLAIN_RING_FRAMES;
	}
	return ozzy_isoc_input_start(&ue->input, e->bus, &in);
}

/* --- MIDI: USB-MIDI event packets (usb_midi_engine does the same on the Xone:1D) --- */

static uint8_t *midi_read_slot(struct ploytec_us3xx_engine *ue, uint32_t slot)
{
	return ue->base.mem.midi_in + slot * MIDI_READ_SIZE;
}

static void submit_midi_read(struct ploytec_us3xx_engine *ue, uint32_t slot)
{
	if (!ozzy_bus_submit(ue->base.bus, ue->midi_in_addr, midi_read_slot(ue, slot), MIDI_READ_SIZE,
			     (void *)(uintptr_t)slot))
		ploytec_log("us3xx: MIDI input submission failed (slot %u)", slot);
}

/* The event packets of one read into the midi_in ring */
static void midi_read_done(struct ploytec_us3xx_engine *ue, uint32_t slot, uint32_t bytes)
{
	volatile struct ozzy_shared_memory *shm = ue->base.shm;
	const uint8_t *packets = midi_read_slot(ue, slot);
	uint32_t offset;

	for (offset = 0; offset + USB_MIDI_PACKET_SIZE <= bytes; offset += USB_MIDI_PACKET_SIZE) {
		uint8_t midi[3];
		unsigned int len = usb_midi_decode_packet(packets + offset, midi);

		if (ozzy_midi_ring_push(&shm->midi_in, midi, len) < len)
			return;   /* ring full: drop the rest */
	}
}

/*
 * pump_midi_out - The next OUT transfer from the midi_out ring: as many whole event packets as
 * the endpoint takes (16 bytes: four). One transfer is in flight at a time.
 */
static void pump_midi_out(struct ploytec_us3xx_engine *ue)
{
	volatile struct ozzy_shared_memory *shm = ue->base.shm;
	uint32_t max = ue->midi_out_max_packet < OZZY_MIDI_OUT_SIZE ? ue->midi_out_max_packet : OZZY_MIDI_OUT_SIZE;

	if (!shm || !ue->midi_out_addr)
		return;
	max -= max % USB_MIDI_PACKET_SIZE;
	if (!max)
		max = USB_MIDI_PACKET_SIZE;
	for (;;) {
		uint8_t *buf = ue->base.mem.midi_out;
		uint32_t r, w, n = 0;

		if (ozzy_xchg_acquire_bool(&ue->midi_out_busy, true))
			return;

		r = shm->midi_out.read_index;
		w = ozzy_load_acquire_u32(&shm->midi_out.write_index);
		while (r != w && n < max) {
			uint8_t byte = shm->midi_out.buffer[r];

			r = (r + 1) & OZZY_MIDI_RING_MASK;
			if (usb_midi_encode_byte(&ue->encoder, CABLE, byte, buf + n))
				n += USB_MIDI_PACKET_SIZE;
		}
		ozzy_store_release_u32(&shm->midi_out.read_index, r);
		if (n) {
			if (ozzy_bus_submit(ue->base.bus, ue->midi_out_addr, buf, n, NULL))
				return;
			ploytec_log("us3xx: MIDI output submission failed");
		}
		ozzy_store_release_bool(&ue->midi_out_busy, false);

		/* A kick that came in while we held midi_out_busy returned early; pick up its bytes */
		if (ozzy_load_acquire_u32(&shm->midi_out.write_index) == r)
			return;
	}
}

/* --- The engine --- */

static bool us3xx_start(struct ozzy_engine *e)
{
	struct ploytec_us3xx_engine *ue = to_us3xx(e);
	volatile struct ozzy_shared_memory *shm = e->shm;
	const struct ozzy_device *model = e->model;
	struct ploytec_us3xx_info hs;
	struct ozzy_clock_status clock = { 0 };
	uint32_t in_channels, out_channels, slot;

	if (!shm)
		return false;
	if (!ue->out_addr || !ue->in_addr) {
		ploytec_log("us3xx: PCM %s endpoint not found", ue->out_addr ? "in" : "out");
		return false;
	}

	/* 1. What the HAL needs: plain frame rings both ways */
	shm->audio.device_flags = OZZY_DEVICE_FLAG_PLAIN_OUTPUT | OZZY_DEVICE_FLAG_PLAIN_INPUT;
	shm->audio.output_ring_frames = OZZY_PLAIN_RING_FRAMES;
	shm->audio.input_ring_frames = OZZY_PLAIN_RING_FRAMES;
	shm->audio.timestamp_frames = e->anchors ? ozzy_anchor_frames(e->settings.value[OZZY_SETTING_IO_BUFFER_MAX], 1) : 0;
	shm->audio.output_bytes_per_frame = model->out_channels * 3;
	shm->audio.input_bytes_per_frame = model->in_channels * 3;
	shm->audio.input_channels = model->in_channels;
	shm->audio.output_channels = model->out_channels;
	ploytec_log("model: %s, %u in / %u out", model->name, model->in_channels, model->out_channels);

	/* 2. The handshake; a restart settles first and selects the interfaces again */
	if (ue->streamed)
		ploytec_restart_settle(e);
	memset(&hs, 0, sizeof(hs));
	if (ploytec_us3xx_handshake(&ploytec_bus_host_ops, (void *)e->bus, ozzy_engine_start_rate(e, ue->default_rate),
				    !ue->mixer_sent, &hs) < 0)
		return false;
	ue->mixer_sent = true;
	/* The mixer mode sets the channels; only the one in the capture is in the device table */
	ploytec_us366_channels(hs.config, &in_channels, &out_channels);
	if (in_channels != model->in_channels || out_channels != model->out_channels) {
		ploytec_log("us3xx: the device's mixer mode (0x%02X) sends %u in / %u out, Ozzy runs %u in / %u out only",
			    hs.config, in_channels, out_channels, model->in_channels, model->out_channels);
		return false;
	}
	ue->sample_rate = hs.rate;
	shm->audio.sample_rate = ue->sample_rate;
	shm->audio.safety_offset_frames = ozzy_isoc_output_safety_offset(ue->sample_rate);
	shm->audio.input_safety_offset_frames = ozzy_isoc_input_safety_offset(ue->sample_rate);
	ozzy_engine_publish_settings(e, &clock);
	e->report.firmware_valid = true;
	e->report.firmware_chip = hs.firmware.chip_id;
	e->report.firmware[0] = 1;
	e->report.firmware[1] = hs.firmware.major;
	e->report.firmware[2] = hs.firmware.minor;
	e->report.boot_rate = hs.boot_rate;
	e->report.input_selector_valid = true;
	e->report.input_selector = ploytec_us3xx_selector(hs.selector, hs.config);
	e->report.output = OZZY_INFO_OUTPUT_ISOC;
	e->report.feedback = OZZY_INFO_FEEDBACK_NONE;
	e->report.in_flight[OZZY_PIPE_PCM_OUT] = ISOC_OUT_TRANSFERS;
	e->report.in_flight[OZZY_PIPE_PCM_IN] = ISOC_IN_TRANSFERS;
	e->report.in_flight[OZZY_PIPE_MIDI_IN] = ue->midi_in_addr ? PLOYTEC_US3XX_MIDI_READS : 0;
	e->report.in_flight[OZZY_PIPE_MIDI_OUT] = ue->midi_out_addr ? 1 : 0;

	ozzy_store_release_bool(&shm->audio.hardware_present, true);
	shm->audio.timestamp.sample_time = 0;
	memset(&ue->encoder, 0, sizeof(ue->encoder));
	ozzy_store_release_bool(&ue->midi_out_busy, false);
	ue->running = true;
	ue->streamed = true;

	/* 3. Transfers in the Windows driver's order: PCM in, PCM out, MIDI in */
	if (!start_input(ue) || !start_output(ue))
		return false;
	for (slot = 0; ue->midi_in_addr && slot < PLOYTEC_US3XX_MIDI_READS; slot++) {
		ue->midi_read_errors[slot] = 0;
		submit_midi_read(ue, slot);
	}

	ozzy_store_release_bool(&shm->audio.driver_ready, true);
	return true;
}

static void us3xx_halt(struct ozzy_engine *e)
{
	struct ploytec_us3xx_engine *ue = to_us3xx(e);

	ue->running = false;
	ozzy_isoc_output_stop(&ue->output);
	ozzy_isoc_input_stop(&ue->input);
}

static void us3xx_suspend(struct ozzy_engine *e)
{
	us3xx_halt(e);
	ploytec_log("us3xx engine suspended");
}

static void us3xx_stop(struct ozzy_engine *e)
{
	volatile struct ozzy_shared_memory *shm = e->shm;

	us3xx_halt(e);
	if (!shm)
		return;
	shm->audio.driver_ready = false;
	shm->audio.hardware_present = false;
	ploytec_log("us3xx engine stopped");
}

static void us3xx_isoc_complete(struct ozzy_engine *e, uint8_t pipe, void *xfer, int status,
				struct ozzy_isoc_packet *packets, uint32_t count)
{
	struct ploytec_us3xx_engine *ue = to_us3xx(e);

	if (!e->bus || !ue->running)
		return;
	if (!ozzy_isoc_input_complete(&ue->input, pipe, xfer, status, packets, count))
		ozzy_isoc_output_complete(&ue->output, pipe, xfer, status, packets, count);
}

static void us3xx_complete(struct ozzy_engine *e, uint8_t pipe, void *xfer, int status, uint32_t bytes)
{
	struct ploytec_us3xx_engine *ue = to_us3xx(e);

	if (!e->bus || !ue->running || status == OZZY_STATUS_ABORTED)
		return;

	if (pipe == ue->midi_in_addr) {
		uint32_t slot = (uint32_t)(uintptr_t)xfer;

		if (slot >= PLOYTEC_US3XX_MIDI_READS)
			return;
		if (status != 0) {
			if (++ue->midi_read_errors[slot] >= MAX_READ_ERRORS) {
				ploytec_log("us3xx: MIDI input slot %u stopped after %u errors (last 0x%x)", slot,
					    ue->midi_read_errors[slot], status);
				return;
			}
		} else {
			ue->midi_read_errors[slot] = 0;
			midi_read_done(ue, slot, bytes);
		}
		submit_midi_read(ue, slot);
	} else if (pipe == ue->midi_out_addr) {
		if (status != 0)
			ploytec_log("us3xx: MIDI output error 0x%x", status);
		ozzy_store_release_bool(&ue->midi_out_busy, false);
		pump_midi_out(ue);
	}
}

static void us3xx_midi_out_pending(struct ozzy_engine *e)
{
	struct ploytec_us3xx_engine *ue = to_us3xx(e);

	if (ue->running)
		pump_midi_out(ue);
}

/* A transfer's microframes of frames at the highest rate the device lists */
static uint32_t us3xx_transfer_frames(const struct ozzy_engine *e, enum ozzy_pcm_dir dir)
{
	uint32_t rates[OZZY_MAX_RATES];
	uint32_t top = rates[ozzy_device_rates(e->model, rates) - 1];

	return top * (dir == OZZY_PCM_IN ? ISOC_IN_UFRAMES : ISOC_TRANSFER_UFRAMES) / ISOC_FB_UFRAMES_PER_SEC;
}

static uint32_t us3xx_setting_quirks(const struct ozzy_engine *e)
{
	return e->model->quirks | (e->anchors ? OZZY_QUIRK_CLOCK_ANCHORS : 0);
}

static const struct ozzy_engine_ops ploytec_us3xx_engine_ops = {
	us3xx_set_endpoint,
	us3xx_start,
	us3xx_stop,
	us3xx_suspend,
	us3xx_complete,
	us3xx_isoc_complete,
	us3xx_midi_out_pending,
	NULL,                   /* timer */
	us3xx_setting_quirks,
	us3xx_transfer_frames,
	NULL,                   /* setting_default */
};

/* Interfaces 0-3 (0 first: Linux probes it and claims the rest), PCM at alternate setting 2 */
void ploytec_us3xx_engine_init(struct ploytec_us3xx_engine *ue, const struct ozzy_device *model)
{
	struct ozzy_device_profile *p = &ue->base.profile;
	uint32_t rates[OZZY_MAX_RATES];

	ue->base.ops = &ploytec_us3xx_engine_ops;
	ue->base.model = model;
	ue->default_rate = ozzy_device_has_rate(model, 48000) ? 48000 : rates[ozzy_device_rates(model, rates) - 1];
	ue->sample_rate = ue->default_rate;
	ue->out_interval = ue->in_interval = 1;

	p->interface_count = PLOYTEC_US3XX_NUM_INTERFACES;
	p->interfaces[0] = 0;
	p->interfaces[1] = PLOYTEC_US3XX_INTF_PCM_OUT;
	p->interfaces[2] = PLOYTEC_US3XX_INTF_PCM_IN;
	p->interfaces[3] = PLOYTEC_US3XX_INTF_MIDI;
	p->alt_settings[1] = PLOYTEC_US3XX_ALT_PCM;
	p->alt_settings[2] = PLOYTEC_US3XX_ALT_PCM;
	p->pipes[OZZY_PIPE_PCM_OUT].address = PLOYTEC_US3XX_EP_PCM_OUT;
	p->pipes[OZZY_PIPE_PCM_OUT].intf = PLOYTEC_US3XX_INTF_PCM_OUT;
	p->pipes[OZZY_PIPE_PCM_IN].address = PLOYTEC_US3XX_EP_PCM_IN;
	p->pipes[OZZY_PIPE_PCM_IN].intf = PLOYTEC_US3XX_INTF_PCM_IN;
	p->pipes[OZZY_PIPE_MIDI_IN].address = PLOYTEC_US3XX_EP_MIDI_IN;
	p->pipes[OZZY_PIPE_MIDI_IN].intf = PLOYTEC_US3XX_INTF_MIDI;
	p->pipes[OZZY_PIPE_MIDI_OUT].address = PLOYTEC_US3XX_EP_MIDI_OUT;
	p->pipes[OZZY_PIPE_MIDI_OUT].intf = PLOYTEC_US3XX_INTF_MIDI;
}
