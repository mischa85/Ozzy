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


/* Endpoints (direction bit OR'd with the base address from ploytec_defs.h) */
#define EP_PCM_OUT   PLOYTEC_EP_PCM_OUT
#define EP_PCM_IN    (PLOYTEC_EP_PCM_IN | 0x80)
#define EP_MIDI_IN   (PLOYTEC_EP_MIDI_IN | 0x80)

/* --- Host services for the shared handshake (common/devices/ploytec/ploytec_handshake.c) --- */

/* Right after a restart killed the streaming transfers, the DB4 (fw 1.4.1) stalls whichever
 * request comes in the next few ms (usbmon: 'V' at 1 ms, 'I' at 5 ms) and answers again about
 * 100 ms later. A stall on endpoint 0 ends with the next setup packet, and every handshake request
 * is a plain read or write, so ask again: up to 10 times, 20 ms apart. */
static int host_control(void *ctx, uint8_t type, uint8_t req, uint16_t val, uint16_t idx, void *data, uint16_t len)
{
	const struct ozzy_bus *bus = (const struct ozzy_bus *)ctx;
	int attempt;

	for (attempt = 1; attempt <= 10; attempt++) {
		if (bus->ops->control(bus->ctx, type, req, val, idx, data, len)) {
			if (attempt > 1)
				ploytec_log("request 0x%02x answered on attempt %d", req, attempt);
			return len;
		}
		bus->ops->sleep_ms(bus->ctx, 20);
	}
	return -EIO;
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
		ploytec_log("transport: %s, wMaxPacketSize %u (%s layout, %u-byte sub-packets)",
			    transfer_type == 2 ? "bulk" : "interrupt", max_packet_size,
			    pe->out_layout == PLOYTEC_OUT_LAYOUT_BULK ? "bulk" : "interrupt",
			    ploytec_out_subpkt_size(pe->out_layout));
	} else if (role == OZZY_PIPE_PCM_IN) {
		/* Selects the MIDI IN frame above 48 kHz (ploytec_midi.h) */
		pe->pcm_in_int1024 = transfer_type == 3 && max_packet_size == 1024;
	}
}

/* PCM out transfer @t: out_frames / 10 sub-packets at its place */
static void submit_pcm_out(struct ploytec_engine *pe, uint32_t t)
{
	uint8_t *buffer = ozzy_engine_pcm_out_slot(&pe->base, t);
	uint32_t size = ploytec_out_xfer_bytes(pe->out_layout, pe->out_frames);

	if (!ozzy_bus_submit(pe->base.bus, EP_PCM_OUT, buffer, size, (void *)(uintptr_t)t))
		ploytec_log("output submission failed (idx: %u)", t);
}

static void submit_midi_in(struct ploytec_engine *pe, uint32_t slot)
{
	if (!ploytec_submit_midi_in(&pe->base, EP_MIDI_IN, &pe->midi, slot))
		ploytec_log("MIDI input submission failed (slot %u)", slot);
}

static bool ploytec_start(struct ozzy_engine *e)
{
	struct ploytec_engine *pe = to_ploytec(e);
	volatile struct ozzy_shared_memory *shm = e->shm;
	const struct ozzy_device *model = e->model;
	struct ploytec_handshake_info hs;
	bool bulk = pe->out_layout == PLOYTEC_OUT_LAYOUT_BULK;
	uint32_t out_places, in_places, in_step, i;

	if (!shm)
		return false;

	/* 0. The transfer sizes for this run (a change takes a restart), laid out in the rings; the
	 * clock anchors (where the platform reads them) every whole number of out transfers, 8/3 of
	 * the largest IO buffer setting */
	pe->out_frames = ozzy_pcm_frames(false, e->settings.value[OZZY_SETTING_PCM_OUT_FRAMES]);
	pe->in_frames = ozzy_pcm_frames(true, e->settings.value[OZZY_SETTING_PCM_IN_FRAMES]);
	in_step = ozzy_pcm_in_step(e->pipe[OZZY_PIPE_PCM_IN].transfer_type,
				   e->pipe[OZZY_PIPE_PCM_IN].max_packet_size, PLOYTEC_IN_FRAME_SIZE);
	if (in_step && pe->in_frames % in_step) {
		/* A size kept from before (or another device) the in packets would overrun: the next
		 * one up (down at the top of the list), and the setting follows */
		pe->in_frames = (pe->in_frames / in_step + 1) * in_step;
		if (pe->in_frames > OZZY_NUM_PCM_IN_FRAMES * 8)
			pe->in_frames -= in_step;
		ploytec_log("in transfers of %u frames, whole %u-frame packets", pe->in_frames, in_step);
		e->settings.value[OZZY_SETTING_PCM_IN_FRAMES] = (uint8_t)(pe->in_frames / 8 - 1);
	}
	pe->out_queue = ozzy_pcm_queue(e->settings.value[OZZY_SETTING_PCM_OUT_QUEUE]);
	pe->in_queue = ozzy_pcm_queue(e->settings.value[OZZY_SETTING_PCM_IN_QUEUE]);
	out_places = ozzy_engine_pcm_layout(e, OZZY_PCM_OUT, ploytec_out_xfer_bytes(pe->out_layout, pe->out_frames));
	in_places = ozzy_engine_pcm_layout(e, OZZY_PCM_IN, ploytec_in_xfer_bytes(pe->in_frames));
	if (!out_places || !in_places) {
		ploytec_log("PCM transfers of %u / %u frames don't fit", pe->out_frames, pe->in_frames);
		return false;
	}
	pe->anchor_frames = e->anchors ? ozzy_anchor_frames(e->settings.value[OZZY_SETTING_IO_BUFFER_MAX], pe->out_frames) : 0;

	/* 1. The format, as the HAL reads it (the platform has set the device names) */
	shm->audio.device_flags = (bulk ? OZZY_DEVICE_FLAG_PLOYTEC_BULK : 0) |
				  (bulk && (model->quirks & OZZY_QUIRK_SPDIF_MIRROR) ? OZZY_DEVICE_FLAG_SPDIF_MIRROR : 0);
	shm->audio.output_ring_frames = out_places * pe->out_frames;
	shm->audio.input_ring_frames = in_places * pe->in_frames;
	shm->audio.timestamp_frames = pe->anchor_frames;
	shm->audio.output_bytes_per_frame = PLOYTEC_OUT_FRAME_SIZE;
	shm->audio.input_bytes_per_frame = PLOYTEC_IN_FRAME_SIZE;
	shm->audio.input_channels = model->in_channels;
	shm->audio.output_channels = model->out_channels;
	/* Output is encoded when the transfer out_queue before it completes, so the HAL writes that
	 * far ahead of the clock. Input frames are there once their own transfer completes, so the
	 * input offset could be one transfer plus the completion jitter; it stays in_queue
	 * transfers, as it ran before the setting, until hardware shows how large that jitter is. */
	shm->audio.safety_offset_frames = pe->out_queue * pe->out_frames;      /* the rate's floor below */
	shm->audio.input_safety_offset_frames = pe->in_queue * pe->in_frames;
	ploytec_log("model: %s, %u in / %u out, transfers of %u frames out, %u in", model->name, model->in_channels,
		    model->out_channels, pe->out_frames, pe->in_frames);

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
	/* The offsets as the host is told them: at least the jitter floor at the rate run */
	shm->audio.safety_offset_frames = ozzy_engine_safety_offset(shm->audio.safety_offset_frames, pe->sample_rate);
	shm->audio.input_safety_offset_frames = ozzy_engine_safety_offset(shm->audio.input_safety_offset_frames,
									  pe->sample_rate);
	ozzy_engine_publish_settings(e, &hs.clock);
	ploytec_report(e, &hs);
	e->report.output = bulk ? OZZY_INFO_OUTPUT_PLOYTEC_BULK : OZZY_INFO_OUTPUT_PLOYTEC_INTERRUPT;
	e->report.in_flight[OZZY_PIPE_PCM_OUT] = (uint8_t)pe->out_queue;
	e->report.in_flight[OZZY_PIPE_PCM_IN] = (uint8_t)pe->in_queue;
	e->report.in_flight[OZZY_PIPE_MIDI_IN] = PLOYTEC_MIDI_IN_XFERS;
	ploytec_midi_init(&pe->midi, pe->sample_rate, pe->pcm_in_int1024);
	pe->encode = ploytec_frame_encoder(model->out_channels);
	pe->decode = ploytec_frame_decoder(model->in_channels);
	pe->mirror = bulk && (model->quirks & OZZY_QUIRK_SPDIF_MIRROR) && model->out_channels == 8;

	/* The device answers (release: the format fields above first) */
	ozzy_store_release_bool(&shm->audio.hardware_present, true);

	/* 3. The clock from 0 */
	pe->hw_sample_time = 0;
	shm->audio.timestamp.sample_time = 0;

	/* 4. Every PCM out place: silence and idle MIDI bytes */
	for (i = 0; i < out_places; i++)
		ploytec_out_init_subpkts(pe->out_layout, ozzy_engine_pcm_out_slot(e, i),
					 pe->out_frames / PLOYTEC_FRAMES_PER_OUT_SUBPKT);

	/* 5. in_queue / out_queue transfers in flight per PCM pipe, PLOYTEC_MIDI_IN_XFERS MIDI IN reads */
	pe->running = true;
	for (i = 0; i < pe->in_queue || i < pe->out_queue; i++) {
		if (i < pe->in_queue && !ploytec_submit_pcm_in(e, EP_PCM_IN, pe->in_frames, i))
			ploytec_log("input submission failed (idx: %u)", i);
		if (i < pe->out_queue)
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
 * midi_out - As many bytes from the midi_out ring as the MIDI out pace allows in transfer @t's
 * sub-packets; slots without data stay idle.
 */
static void midi_out(struct ploytec_engine *pe, uint32_t t)
{
	volatile struct ozzy_shared_memory *shm = pe->base.shm;
	uint8_t bytes[PLOYTEC_MAX_OUT_SUBPKTS];
	uint32_t mask, subpkts = pe->out_frames / PLOYTEC_FRAMES_PER_OUT_SUBPKT;
	uint32_t count = ploytec_midi_out_slots(&pe->midi, subpkts, &mask);
	uint8_t *packet = ozzy_engine_pcm_out_slot(&pe->base, t);

	memset(bytes, PLOYTEC_MIDI_IDLE_BYTE, sizeof(bytes));
	ozzy_midi_ring_pop(&shm->midi_out, bytes, count);
	ploytec_midi_out_place(pe->out_layout, packet, subpkts, mask, bytes);
}

/*
 * encode_pcm_out - PCM out from the attached ring: transfer @t's frames, encoded straight from
 * the ring into the transfer about to be sent (the MIDI and sync bytes are already in place).
 * Frame offsets count from the transfer's first sub-packet.
 */
static void encode_pcm_out(struct ploytec_engine *pe, uint32_t t)
{
	const struct ozzy_pcm_ring *ring = ozzy_engine_take_pcm_ring(&pe->base, OZZY_PCM_OUT);
	uint8_t *packet = ozzy_engine_pcm_out_slot(&pe->base, t);
	uint32_t frame_bytes, r, f;

	if (!ring || !pe->encode) {
		/* A platform with rings and none attached (stopped): silence, as Linux's
		 * init_out_urb, not the last audio encoded into this slot */
		if (ozzy_engine_ring_platform(&pe->base))
			for (f = 0; f < pe->out_frames; f++)
				memset(packet + ploytec_out_frame_offset(pe->out_layout, f), 0, PLOYTEC_OUT_FRAME_SIZE);
		return;
	}
	frame_bytes = ring->channels * 3;
	r = ozzy_mod_u64(pe->base.pcm_pos[OZZY_PCM_OUT], ring->frames);
	for (f = 0; f < pe->out_frames; f++) {
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
	 * last frame of the transfer before, already encoded */
	if (pe->mirror) {
		unsigned int pair = pe->base.settings.value[OZZY_SETTING_SPDIF_MIRROR];
		uint32_t before = ploytec_pcm_prev(t, pe->base.mem.pcm_out_slots);
		const uint8_t *prev = ozzy_engine_pcm_out_slot(&pe->base, before) +
				      ploytec_out_frame_offset(pe->out_layout, pe->out_frames - 1);

		for (f = 0; f < pe->out_frames; f++) {
			uint8_t *dst = packet + ploytec_out_frame_offset(pe->out_layout, f);

			ploytec_spdif_mirror(dst, prev + PLOYTEC_OUT_FRAME_SIZE / 2, pair);
			prev = dst;
		}
	}

	pe->base.pcm_pos[OZZY_PCM_OUT] += pe->out_frames;
	ozzy_bus_pcm_progress(pe->base.bus, OZZY_PCM_OUT, pe->base.pcm_pos[OZZY_PCM_OUT]);
}

/*
 * update_timestamp - The device's clock: a transfer's frames played. Every anchor_frames an
 * anchor (ozzy_timestamp_write; reader: OzzyHAL::GetZeroTimeStamp), a whole number of
 * transfers, so the clock lands on it; none where the platform doesn't read them (0).
 */
static void update_timestamp(struct ploytec_engine *pe)
{
	volatile struct ozzy_shared_memory *shm = pe->base.shm;

	if (!shm->audio.driver_ready || !pe->anchor_frames)
		return;
	pe->hw_sample_time += pe->out_frames;
	if (ozzy_mod_u64(pe->hw_sample_time, pe->anchor_frames))
		return;

	ozzy_timestamp_write(&shm->audio.timestamp, pe->hw_sample_time, ozzy_bus_time(pe->base.bus));
}

static void ploytec_complete(struct ozzy_engine *e, uint8_t pipe, void *xfer, int status, uint32_t bytes)
{
	struct ploytec_engine *pe = to_ploytec(e);
	volatile struct ozzy_shared_memory *shm = e->shm;
	uint32_t finished = (uint32_t)(uintptr_t)xfer;
	uint32_t next;

	/* Aborted (suspend/stop): no resubmit, a synchronous abort would accept it */
	if (!e->bus || !pe->running || status == OZZY_STATUS_ABORTED)
		return;

	if (pipe == EP_PCM_OUT) {
		next = ploytec_pcm_next(finished, pe->out_queue, e->mem.pcm_out_slots);
		update_timestamp(pe);
		midi_out(pe, next);
		encode_pcm_out(pe, next);
		submit_pcm_out(pe, next);
	} else if (pipe == EP_PCM_IN) {
		next = ploytec_pcm_next(finished, pe->in_queue, e->mem.pcm_in_slots);
		ploytec_decode_pcm_in(e, pe->decode, pe->in_frames, finished, status == 0 ? bytes : 0);
		if (!ploytec_submit_pcm_in(e, EP_PCM_IN, pe->in_frames, next))
			ploytec_log("input submission failed (idx: %u)", next);
	} else if (pipe == EP_MIDI_IN) {
		/* Only the MIDI bytes of the frame (ploytec_midi.h); the slot is the transfer's */
		if (bytes)
			ploytec_midi_in_to_ring(&pe->midi, shm, ploytec_midi_in_slot(e, finished),
						bytes < PLOYTEC_MIDI_IN_SLOT ? bytes : PLOYTEC_MIDI_IN_SLOT);
		submit_midi_in(pe, finished);
	}
}

/* The ESU1808's mirror only on the bulk layout (official dmaEncode08_Int964 on interrupt); the
 * transfer sizes and queues on every device of the family */
static uint32_t ploytec_setting_quirks(const struct ozzy_engine *e)
{
	const struct ploytec_engine *pe = (const struct ploytec_engine *)e;
	uint32_t quirks = e->model->quirks | OZZY_QUIRK_PCM_OUT_FRAMES | OZZY_QUIRK_PCM_IN_FRAMES |
			  OZZY_QUIRK_PCM_OUT_QUEUE | OZZY_QUIRK_PCM_IN_QUEUE |
			  (e->anchors ? OZZY_QUIRK_CLOCK_ANCHORS : 0);

	return pe->out_layout == PLOYTEC_OUT_LAYOUT_BULK ? quirks : (quirks & ~(uint32_t)OZZY_QUIRK_SPDIF_MIRROR);
}

/* A PCM transfer's frames this run: the smallest period a platform may ask for */
static uint32_t ploytec_transfer_frames(const struct ozzy_engine *e, enum ozzy_pcm_dir dir)
{
	const struct ploytec_engine *pe = (const struct ploytec_engine *)e;

	return dir == OZZY_PCM_OUT ? pe->out_frames : pe->in_frames;
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
	ploytec_transfer_frames,
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
	pe->out_frames = ozzy_pcm_frames(false, ozzy_engine_setting_default(&pe->base, OZZY_SETTING_PCM_OUT_FRAMES));
	pe->in_frames = ozzy_pcm_frames(true, ozzy_engine_setting_default(&pe->base, OZZY_SETTING_PCM_IN_FRAMES));
	pe->out_queue = pe->in_queue = ozzy_pcm_queue(OZZY_NUM_PCM_QUEUES - 1);
	pe->anchor_frames = 0;   /* start() sets it, where the platform reads anchors */

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
