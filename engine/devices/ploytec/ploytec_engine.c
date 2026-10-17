/* SPDX-License-Identifier: MIT */
/*
 * Ploytec junction devices with bulk or interrupt PCM out (ploytec_engine.h).
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "ploytec_engine.h"
#include "ploytec_io.h"
#include "../../../common/devices/ploytec/ploytec_handshake.h"
#include "../../shared/ozzy_log.h"

/* PCM transfers in flight per pipe, as Linux runs them (OZZY_PCM_N_URBS): margin under load */
#define PCM_XFERS    4

/* Endpoints (direction bit OR'd with the base address from ploytec_defs.h) */
#define EP_PCM_OUT   PLOYTEC_EP_PCM_OUT
#define EP_PCM_IN    (PLOYTEC_EP_PCM_IN | 0x80)
#define EP_MIDI_IN   (PLOYTEC_EP_MIDI_IN | 0x80)

/* --- Host services for the shared handshake (common/devices/ploytec/ploytec_handshake.c) --- */

static int host_control(void *ctx, uint8_t type, uint8_t req, uint16_t val, uint16_t idx, void *data, uint16_t len)
{
	const struct ozzy_bus *bus = (const struct ozzy_bus *)ctx;

	return bus->ops->control(bus->ctx, type, req, val, idx, data, len) ? len : -EIO;
}

static int host_clear_halt(void *ctx, uint8_t endpoint)
{
	const struct ozzy_bus *bus = (const struct ozzy_bus *)ctx;

	if (!bus->ops->clear_halt) {
		ploytec_log("no clear halt on this platform (endpoint 0x%02x)", endpoint);
		return 0;
	}
	return bus->ops->clear_halt(bus->ctx, endpoint);
}

static void host_sleep(void *ctx, unsigned int ms)
{
	const struct ozzy_bus *bus = (const struct ozzy_bus *)ctx;

	bus->ops->sleep_ms(bus->ctx, ms);
}

static void host_log(void *ctx, bool error, const char *fmt, va_list args)
{
	ploytec_vlog(fmt, args);
}

/* The Xones clear the PCM endpoint halts before streaming, as the official Windows driver does
 * and Linux does: without it the 4D never starts streaming (#75, ploytec_handshake.c) */
static const struct ploytec_host_ops xone_host_ops = { host_control, host_clear_halt, host_sleep, host_log };

/* Without clear_halt, for ploytec_isoc_engine: the official driver doesn't clear halts there */
const struct ploytec_host_ops ploytec_bus_host_ops = { host_control, NULL, host_sleep, host_log };

static struct ploytec_engine *to_ploytec(struct ozzy_engine *e)
{
	return (struct ploytec_engine *)e;
}

/*
 * The output packet layout follows the endpoint's wMaxPacketSize, not its transfer type, like
 * the official driver (AudioBulkIRP::useOut): <= 512 uses the bulk layout, larger (964) the
 * interrupt layout. DB2 fw 1.2.0 is an interrupt endpoint with wMaxPacketSize 512.
 */
static void ploytec_set_endpoint(struct ozzy_engine *e, enum ozzy_pipe_role role, uint8_t address,
				 uint8_t transfer_type, uint16_t max_packet_size, uint8_t interval)
{
	struct ploytec_engine *pe = to_ploytec(e);

	if (role == OZZY_PIPE_PCM_OUT) {
		pe->out_layout = ploytec_out_layout_for_maxpacket(max_packet_size);
		pe->packet_size_out = ploytec_out_packet_size(pe->out_layout);
		ploytec_log("transport: %s, wMaxPacketSize %u (%s layout, out packet: %u bytes)",
			    transfer_type == 2 ? "bulk" : "interrupt", max_packet_size,
			    pe->out_layout == PLOYTEC_OUT_LAYOUT_BULK ? "bulk" : "interrupt", pe->packet_size_out);
	} else if (role == OZZY_PIPE_PCM_IN) {
		/* Selects the MIDI IN frame above 48 kHz (ploytec_midi.h) */
		pe->pcm_in_int1024 = transfer_type == 3 && max_packet_size == 1024;
	}
}

static void submit_pcm_out(struct ploytec_engine *pe, uint32_t idx)
{
	volatile struct ozzy_shared_memory *shm = pe->base.shm;
	uint8_t *buffer = (uint8_t *)shm->audio.output_buffer + (idx % OZZY_NUM_PACKETS) * OZZY_MAX_PACKET_SIZE;

	if (!ozzy_bus_submit(pe->base.bus, EP_PCM_OUT, buffer, pe->packet_size_out, (void *)(uintptr_t)idx))
		ploytec_log("output submission failed (idx: %u)", idx);
}

static void submit_midi_in(struct ploytec_engine *pe, uint32_t slot)
{
	if (!ploytec_submit_midi_in(pe->base.bus, pe->base.shm, EP_MIDI_IN, &pe->midi, slot))
		ploytec_log("MIDI input submission failed (slot %u)", slot);
}

static bool ploytec_start(struct ozzy_engine *e)
{
	struct ploytec_engine *pe = to_ploytec(e);
	volatile struct ozzy_shared_memory *shm = e->shm;
	const struct ozzy_device *model = e->model;
	struct ploytec_handshake_info hs;
	bool bulk = pe->out_layout == PLOYTEC_OUT_LAYOUT_BULK;
	uint32_t i;

	if (!shm)
		return false;

	/* 1. The format, as the HAL reads it (the platform has set the device names) */
	shm->audio.device_flags = (bulk ? OZZY_DEVICE_FLAG_PLOYTEC_BULK : 0) |
				  (bulk && (model->quirks & OZZY_QUIRK_SPDIF_MIRROR) ? OZZY_DEVICE_FLAG_SPDIF_MIRROR : 0);
	shm->audio.frames_per_packet = PLOYTEC_FRAMES_PER_PKT;
	shm->audio.samples_per_packet = PLOYTEC_FRAMES_PER_OUT_SUBPKT;
	shm->audio.output_bytes_per_frame = PLOYTEC_OUT_FRAME_SIZE;
	shm->audio.input_bytes_per_frame = PLOYTEC_IN_FRAME_SIZE;
	shm->audio.input_channels = model->in_channels;
	shm->audio.output_channels = model->out_channels;
	/* Output is encoded when the transfer PCM_XFERS before it completes, so the HAL writes
	 * that far ahead of the clock */
	shm->audio.safety_offset_frames = PCM_XFERS * PLOYTEC_FRAMES_PER_PKT;
	ploytec_log("model: %s, %u in / %u out", model->name, model->in_channels, model->out_channels);

	/* 2. The handshake, with the settings; locked to S/PDIF it sets the lock rate */
	memset(&hs, 0, sizeof(hs));
	if (ploytec_handshake(&xone_host_ops, (void *)e->bus, ozzy_engine_start_rate(e, pe->default_rate), model->quirks,
			      &e->settings, &hs) < 0)
		return false;
	if (!ozzy_device_has_rate(model, hs.rate)) {
		/* Locked to an S/PDIF rate the device doesn't list (Linux: ploytec_adopt_rate) */
		ploytec_log("device runs at %u Hz, which it doesn't list", hs.rate);
		return false;
	}
	pe->sample_rate = hs.rate;
	shm->audio.sample_rate = pe->sample_rate;
	ozzy_engine_publish_settings(e, &hs.clock);
	ploytec_midi_init(&pe->midi, pe->sample_rate, pe->pcm_in_int1024);
	pe->encode = ploytec_frame_encoder(model->out_channels);
	pe->decode = ploytec_frame_decoder(model->in_channels);
	pe->mirror = bulk && (model->quirks & OZZY_QUIRK_SPDIF_MIRROR) && model->out_channels == 8;

	/* The device answers (release: the format fields above first) */
	ozzy_store_release_bool(&shm->audio.hardware_present, true);

	/* 3. The clock from 0 */
	pe->hw_sample_time = 0;
	shm->audio.timestamp.sample_time = 0;

	/* 4. Every logical packet: silence and idle MIDI bytes */
	for (i = 0; i < OZZY_NUM_PACKETS; i++)
		ploytec_out_init_packet(pe->out_layout, (uint8_t *)shm->audio.output_buffer + i * OZZY_MAX_PACKET_SIZE);

	/* 5. PCM_XFERS transfers in flight per PCM pipe, PLOYTEC_MIDI_IN_XFERS MIDI IN reads */
	pe->running = true;
	for (i = 0; i < PCM_XFERS; i++) {
		if (!ploytec_submit_pcm_in(e->bus, shm, EP_PCM_IN, i))
			ploytec_log("input submission failed (idx: %u)", i);
		submit_pcm_out(pe, i);
	}
	for (i = 0; i < PLOYTEC_MIDI_IN_XFERS; i++)
		submit_midi_in(pe, i);

	ozzy_store_release_bool(&shm->audio.driver_ready, true);
	return true;
}

static void ploytec_suspend(struct ozzy_engine *e)
{
	to_ploytec(e)->running = false;
	ploytec_log("engine suspended");
}

static void ploytec_stop(struct ozzy_engine *e)
{
	volatile struct ozzy_shared_memory *shm = e->shm;

	to_ploytec(e)->running = false;
	if (!shm)
		return;
	/* The HAL drops the device */
	shm->audio.driver_ready = false;
	shm->audio.hardware_present = false;
	ploytec_log("engine stopped");
}

/*
 * midi_out - As many bytes from the midi_out ring as the MIDI out pace allows in packet @idx;
 * slots without data stay idle.
 */
static void midi_out(struct ploytec_engine *pe, uint32_t idx)
{
	volatile struct ozzy_shared_memory *shm = pe->base.shm;
	uint8_t bytes[PLOYTEC_OUT_SUBPKTS];
	uint32_t mask;
	uint32_t count = ploytec_midi_out_slots(&pe->midi, &mask);
	uint8_t *packet = (uint8_t *)shm->audio.output_buffer + (idx % OZZY_NUM_PACKETS) * OZZY_MAX_PACKET_SIZE;

	memset(bytes, PLOYTEC_MIDI_IDLE_BYTE, sizeof(bytes));
	ozzy_midi_ring_pop(&shm->midi_out, bytes, count);
	ploytec_midi_out_place(pe->out_layout, packet, mask, bytes);
}

/*
 * encode_pcm_out - PCM out from the attached ring: packet @idx's 80 frames, encoded straight
 * from the ring into the packet about to be sent (the MIDI and sync bytes are already in place).
 */
static void encode_pcm_out(struct ploytec_engine *pe, uint32_t idx)
{
	const struct ozzy_pcm_ring *ring = ozzy_engine_take_pcm_ring(&pe->base, OZZY_PCM_OUT);
	uint8_t *packet = (uint8_t *)pe->base.shm->audio.output_buffer + (idx % OZZY_NUM_PACKETS) * OZZY_MAX_PACKET_SIZE;
	uint32_t frame_bytes, r, f;

	if (!ring || !pe->encode) {
		/* A platform with rings and none attached (stopped): silence, as Linux's
		 * init_out_urb, not the last audio encoded into this slot */
		if (ozzy_engine_ring_platform(&pe->base))
			for (f = 0; f < PLOYTEC_FRAMES_PER_PKT; f++)
				memset(packet + ploytec_out_frame_offset(pe->out_layout, f), 0, PLOYTEC_OUT_FRAME_SIZE);
		return;
	}
	frame_bytes = ring->channels * 3;
	r = ozzy_mod_u64(pe->base.pcm_pos[OZZY_PCM_OUT], ring->frames);
	for (f = 0; f < PLOYTEC_FRAMES_PER_PKT; f++) {
		const uint8_t *src = ring->base + r * frame_bytes;
		uint8_t routed[PLOYTEC_MAX_OUT_CHANNELS * 3];

		if (!pe->base.out_route.identity) {
			ozzy_route_frame(routed, src, &pe->base.out_route, 2 * 3);
			src = routed;
		}
		pe->encode(packet + ploytec_out_frame_offset(pe->out_layout, f), src);
		if (++r == ring->frames)
			r = 0;
	}

	/* The ESU1808's S/PDIF mirror takes lane 9 from the frame before; for frame 0 that is the
	 * last frame of the packet before, already encoded */
	if (pe->mirror) {
		unsigned int pair = pe->base.settings.value[OZZY_SETTING_SPDIF_MIRROR];
		const uint8_t *prev = (uint8_t *)pe->base.shm->audio.output_buffer +
				      ((idx + OZZY_NUM_PACKETS - 1) % OZZY_NUM_PACKETS) * OZZY_MAX_PACKET_SIZE +
				      ploytec_out_frame_offset(pe->out_layout, PLOYTEC_FRAMES_PER_PKT - 1);

		for (f = 0; f < PLOYTEC_FRAMES_PER_PKT; f++) {
			uint8_t *dst = packet + ploytec_out_frame_offset(pe->out_layout, f);

			ploytec_spdif_mirror(dst, prev + PLOYTEC_OUT_FRAME_SIZE / 2, pair);
			prev = dst;
		}
	}

	pe->base.pcm_pos[OZZY_PCM_OUT] += PLOYTEC_FRAMES_PER_PKT;
	ozzy_bus_pcm_progress(pe->base.bus, OZZY_PCM_OUT, pe->base.pcm_pos[OZZY_PCM_OUT]);
}

/*
 * update_timestamp - The device's clock: 80 more frames played. Every OZZY_TIMESTAMP_PACKETS
 * an anchor (seqlock, reader: OzzyHAL::GetZeroTimeStamp); the fences keep the odd sequence
 * visible before the new values, and the new values before the even sequence.
 */
static void update_timestamp(struct ploytec_engine *pe)
{
	volatile struct ozzy_shared_memory *shm = pe->base.shm;
	volatile struct ozzy_timestamp *ts = &shm->audio.timestamp;
	uint32_t seq;

	if (!shm->audio.driver_ready)
		return;
	pe->hw_sample_time += PLOYTEC_FRAMES_PER_PKT;
	if (ozzy_mod_u64(pe->hw_sample_time, PLOYTEC_FRAMES_PER_PKT * OZZY_TIMESTAMP_PACKETS))
		return;

	seq = ts->sequence;
	ozzy_store_relaxed_u32(&ts->sequence, seq + 1);
	ozzy_fence_release();
	ozzy_store_relaxed_u64(&ts->sample_time, pe->hw_sample_time);
	ozzy_store_relaxed_u64(&ts->host_time, ozzy_bus_time(pe->base.bus));
	ozzy_store_release_u32(&ts->sequence, seq + 2);
}

static void ploytec_complete(struct ozzy_engine *e, uint8_t pipe, void *xfer, int status, uint32_t bytes)
{
	struct ploytec_engine *pe = to_ploytec(e);
	volatile struct ozzy_shared_memory *shm = e->shm;
	uint32_t finished = (uint32_t)(uintptr_t)xfer;
	uint32_t next = finished + PCM_XFERS;

	/* Aborted (suspend/stop): no resubmit, a synchronous abort would accept it */
	if (!e->bus || !pe->running || status == OZZY_STATUS_ABORTED)
		return;

	if (pipe == EP_PCM_OUT) {
		update_timestamp(pe);
		midi_out(pe, next);
		encode_pcm_out(pe, next);
		submit_pcm_out(pe, next);
	} else if (pipe == EP_PCM_IN) {
		ploytec_decode_pcm_in(e, pe->decode, finished, status == 0 ? bytes : 0);
		if (!ploytec_submit_pcm_in(e->bus, shm, EP_PCM_IN, next))
			ploytec_log("input submission failed (idx: %u)", next);
	} else if (pipe == EP_MIDI_IN) {
		/* Only the MIDI bytes of the frame (ploytec_midi.h); the slot is the transfer's */
		if (bytes)
			ploytec_midi_in_to_ring(&pe->midi, shm, ploytec_midi_in_slot(shm, finished),
						bytes < PLOYTEC_MIDI_IN_SLOT ? bytes : PLOYTEC_MIDI_IN_SLOT);
		submit_midi_in(pe, finished);
	}
}

/* The ESU1808's mirror only on the bulk layout (official dmaEncode08_Int964 on interrupt) */
static uint32_t ploytec_setting_quirks(const struct ozzy_engine *e)
{
	const struct ploytec_engine *pe = (const struct ploytec_engine *)e;

	return pe->out_layout == PLOYTEC_OUT_LAYOUT_BULK ? e->model->quirks
							 : (e->model->quirks & ~(uint32_t)OZZY_QUIRK_SPDIF_MIRROR);
}

static const struct ozzy_engine_ops ploytec_engine_ops = {
	ploytec_set_endpoint,
	ploytec_start,
	ploytec_stop,
	ploytec_suspend,
	ploytec_complete,
	NULL,                   /* isoc_complete */
	NULL,                   /* midi_out_pending: MIDI goes out with every PCM packet */
	NULL,                   /* timer */
	ploytec_setting_quirks,
};

/*
 * The Xones present two interfaces: 0 has the control requests, PCM out (EP 0x05) and MIDI in
 * (EP 0x83); 1 has PCM in (EP 0x86). Run at the highest rate the device lists (the DB2 is
 * 48 kHz only).
 */
void ploytec_engine_init(struct ploytec_engine *pe, const struct ozzy_device *model)
{
	struct ozzy_device_profile *p = &pe->base.profile;
	uint32_t rates[OZZY_MAX_RATES];

	pe->base.ops = &ploytec_engine_ops;
	pe->base.model = model;
	pe->default_rate = rates[ozzy_device_rates(model, rates) - 1];
	pe->sample_rate = pe->default_rate;
	pe->out_layout = PLOYTEC_OUT_LAYOUT_INT;

	p->interface_count = PLOYTEC_NUM_INTERFACES;
	p->interfaces[0] = 0;
	p->interfaces[1] = 1;
	p->alt_setting = PLOYTEC_ALT_SETTING;
	p->pipes[OZZY_PIPE_PCM_OUT].address = EP_PCM_OUT;
	p->pipes[OZZY_PIPE_PCM_OUT].intf = 0;
	p->pipes[OZZY_PIPE_PCM_IN].address = EP_PCM_IN;
	p->pipes[OZZY_PIPE_PCM_IN].intf = 1;
	p->pipes[OZZY_PIPE_MIDI_IN].address = EP_MIDI_IN;
	p->pipes[OZZY_PIPE_MIDI_IN].intf = 0;
}
