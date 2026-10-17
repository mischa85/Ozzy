/* SPDX-License-Identifier: MIT */
/*
 * USB MIDI class devices (usb_midi_engine.h).
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "usb_midi_engine.h"
#include "../../shared/ozzy_log.h"

/* Xone:1D (lsusb in #27): interface 1 is MIDIStreaming with one embedded jack per direction */
#define MIDI_INTERFACE  1
#define EP_MIDI_IN      0x81
#define EP_MIDI_OUT     0x01
#define CABLE           0

/* Read one event packet per transfer. The endpoint's wMaxPacketSize is 4, so a longer transfer
 * would only complete once it filled up, holding back every event until then. */
#define READ_SIZE       USB_MIDI_PACKET_SIZE

/* A read slot stops after this many errors in a row instead of resubmitting forever */
#define MAX_READ_ERRORS 8

static struct usb_midi_engine *to_midi(struct ozzy_engine *e)
{
	return (struct usb_midi_engine *)e;
}

static void midi_set_endpoint(struct ozzy_engine *e, enum ozzy_pipe_role role, uint8_t address,
			      uint8_t transfer_type, uint16_t max_packet_size, uint8_t interval)
{
	/* Fixed addresses */
}

static void submit_read(struct usb_midi_engine *me, uint32_t slot)
{
	void *buf = me->base.mem.midi_in + slot * OZZY_DMA_ALIGN;

	if (!ozzy_bus_submit(me->base.bus, EP_MIDI_IN, buf, READ_SIZE, (void *)(uintptr_t)slot))
		usb_midi_log("MIDI input submission failed (slot %u)", slot);
}

static bool midi_start(struct ozzy_engine *e)
{
	struct usb_midi_engine *me = to_midi(e);
	volatile struct ozzy_shared_memory *shm = e->shm;
	uint32_t slot;

	if (!shm)
		return false;

	/* The device calls itself "Xone MIDI Device"; use the name on the front panel */
	ozzy_strlcpy((char *)shm->product_name, "Xone:1D", sizeof(shm->product_name));

	/* No audio: the HAL skips devices without channels */
	shm->audio.input_channels = 0;
	shm->audio.output_channels = 0;

	memset(&me->encoder, 0, sizeof(me->encoder));
	ozzy_store_release_bool(&me->out_busy, false);
	me->running = true;
	ozzy_store_release_bool(&shm->audio.hardware_present, true);

	for (slot = 0; slot < USB_MIDI_ENGINE_READS; slot++) {
		me->read_errors[slot] = 0;
		submit_read(me, slot);
	}

	ozzy_store_release_bool(&shm->audio.driver_ready, true);
	usb_midi_log("Xone:1D ready");
	return true;
}

static void midi_stop(struct ozzy_engine *e)
{
	volatile struct ozzy_shared_memory *shm = e->shm;

	to_midi(e)->running = false;
	if (!shm)
		return;
	shm->audio.driver_ready = false;
	shm->audio.hardware_present = false;
	usb_midi_log("engine stopped");
}

/* Unpack the event packets into the midi_in ring that OzzyMIDI reads */
static void process_read(struct usb_midi_engine *me, uint32_t slot, uint32_t bytes)
{
	volatile struct ozzy_shared_memory *shm = me->base.shm;
	const uint8_t *packets = me->base.mem.midi_in + slot * OZZY_DMA_ALIGN;
	uint32_t offset;

	for (offset = 0; offset + USB_MIDI_PACKET_SIZE <= bytes; offset += USB_MIDI_PACKET_SIZE) {
		uint8_t midi[3];
		unsigned int len = usb_midi_decode_packet(packets + offset, midi);

		if (ozzy_midi_ring_push(&shm->midi_in, midi, len) < len)
			return;   /* ring full: drop the rest */
	}
}

/*
 * pump_midi_out - Send the next event packet from the midi_out ring. One OUT transfer is in
 * flight at a time.
 */
static void pump_midi_out(struct usb_midi_engine *me)
{
	volatile struct ozzy_shared_memory *shm = me->base.shm;

	if (!shm)
		return;
	for (;;) {
		uint8_t *packet = me->base.mem.midi_out;
		uint32_t r, w;

		if (ozzy_xchg_acquire_bool(&me->out_busy, true))
			return;

		r = shm->midi_out.read_index;
		w = ozzy_load_acquire_u32(&shm->midi_out.write_index);
		while (r != w) {
			uint8_t byte = shm->midi_out.buffer[r];

			r = (r + 1) & OZZY_MIDI_RING_MASK;
			if (usb_midi_encode_byte(&me->encoder, CABLE, byte, packet)) {
				ozzy_store_release_u32(&shm->midi_out.read_index, r);
				if (ozzy_bus_submit(me->base.bus, EP_MIDI_OUT, packet, USB_MIDI_PACKET_SIZE, NULL))
					return;
				usb_midi_log("MIDI output submission failed");
				break;
			}
		}
		ozzy_store_release_u32(&shm->midi_out.read_index, r);
		ozzy_store_release_bool(&me->out_busy, false);

		/* A kick that came in while we held out_busy returned early; pick up its bytes */
		if (ozzy_load_acquire_u32(&shm->midi_out.write_index) == r)
			return;
	}
}

static void midi_complete(struct ozzy_engine *e, uint8_t pipe, void *xfer, int status, uint32_t bytes)
{
	struct usb_midi_engine *me = to_midi(e);

	if (!e->bus || !me->running)
		return;

	if (pipe == EP_MIDI_IN) {
		uint32_t slot = (uint32_t)(uintptr_t)xfer;

		if (status != 0) {
			if (++me->read_errors[slot] >= MAX_READ_ERRORS) {
				usb_midi_log("MIDI input slot %u stopped after %u errors (last 0x%x)", slot,
					     me->read_errors[slot], status);
				return;
			}
		} else {
			me->read_errors[slot] = 0;
			process_read(me, slot, bytes);
		}
		submit_read(me, slot);
	} else if (pipe == EP_MIDI_OUT) {
		if (status != 0)
			usb_midi_log("MIDI output error 0x%x", status);
		ozzy_store_release_bool(&me->out_busy, false);
		pump_midi_out(me);
	}
}

static void midi_out_pending(struct ozzy_engine *e)
{
	struct usb_midi_engine *me = to_midi(e);

	if (me->running)
		pump_midi_out(me);
}

static const struct ozzy_engine_ops usb_midi_engine_ops = {
	midi_set_endpoint,
	midi_start,
	midi_stop,
	NULL,                   /* suspend: no settings */
	midi_complete,
	NULL,                   /* isoc_complete */
	midi_out_pending,
	NULL,                   /* timer */
	NULL,                   /* setting_quirks */
};

void usb_midi_engine_init(struct usb_midi_engine *me, const struct ozzy_device *model)
{
	struct ozzy_device_profile *p = &me->base.profile;

	me->base.ops = &usb_midi_engine_ops;
	me->base.model = model;
	p->interface_count = 1;
	p->interfaces[0] = MIDI_INTERFACE;
	p->alt_setting = 0;
	p->pipes[OZZY_PIPE_MIDI_IN].address = EP_MIDI_IN;
	p->pipes[OZZY_PIPE_MIDI_IN].intf = MIDI_INTERFACE;
	p->pipes[OZZY_PIPE_MIDI_OUT].address = EP_MIDI_OUT;
	p->pipes[OZZY_PIPE_MIDI_OUT].intf = MIDI_INTERFACE;
}
