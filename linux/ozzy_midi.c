// SPDX-License-Identifier: MIT
/*
 * Ozzy USB Audio Driver - MIDI Subsystem
 *
 * Device-agnostic ALSA rawmidi implementation. Handles URB lifecycle,
 * ALSA rawmidi ops, and the MIDI send buffer. Device-specific MIDI
 * embedding in PCM output packets is handled by the device's fill_midi_out
 * callback, which calls ozzy_midi_consume() to read from the send buffer.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include <linux/slab.h>
#include <linux/version.h>
#include <sound/rawmidi.h>

#include "ozzy.h"
#include "ozzy_log.h"
#include "ozzy_midi.h"

/* ========================================================================
 * MIDI Send Ring (consumed by device's fill_midi_out)
 * ======================================================================== */

#define SEND_MASK (OZZY_MIDI_SEND_BUF_SIZE - 1)

static unsigned int ozzy_midi_send_used(const struct midi_runtime *rt)
{
	return (rt->send_head - rt->send_tail) & SEND_MASK;
}

static void ozzy_midi_send_push(struct midi_runtime *rt, u8 byte)
{
	rt->send_buffer[rt->send_head] = byte;
	rt->send_head = (rt->send_head + 1) & SEND_MASK;
}

/* Data bytes that follow a channel status byte (0x80..0xEF) */
static u8 ozzy_midi_data_len(u8 status)
{
	return ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0) ? 1 : 2;
}

/*
 * ozzy_midi_put_out - Queue one byte from the application, expanding running status.
 *
 * The ALSA sequencer's rawmidi bridge drops a status byte equal to the previous one
 * (seq_midi.c never calls snd_midi_event_no_status()). The official driver only ever
 * sends complete messages (CoreMIDI packets carry no running status), and at least
 * one Ploytec firmware ignores running status (PR #80), so put the status back in
 * front of every channel message. Needs room for 2 bytes.
 */
static void ozzy_midi_put_out(struct midi_runtime *rt, u8 byte)
{
	if (byte >= 0xF8) {
		/* real-time: may appear anywhere, doesn't affect running status */
		ozzy_midi_send_push(rt, byte);
	} else if (byte >= 0xF0) {
		/* SysEx and system common cancel running status; their data passes as is */
		ozzy_midi_send_push(rt, byte);
		rt->run_status = 0;
		rt->run_remaining = 0;
	} else if (byte & 0x80) {
		ozzy_midi_send_push(rt, byte);
		rt->run_status = byte;
		rt->run_remaining = ozzy_midi_data_len(byte);
	} else {
		if (rt->run_status && !rt->run_remaining) {
			ozzy_midi_send_push(rt, rt->run_status);
			rt->run_remaining = ozzy_midi_data_len(rt->run_status);
		}
		ozzy_midi_send_push(rt, byte);
		if (rt->run_remaining)
			rt->run_remaining--;
	}
}

/*
 * ozzy_midi_fill_send - Move pending bytes from the rawmidi output substream into
 * the send ring. Call with out_lock held.
 */
static void ozzy_midi_fill_send(struct midi_runtime *rt)
{
	u8 byte;

	if (!rt->out)
		return;

	/* each byte can grow to 2 (re-inserted status), keep one slot free */
	while (ozzy_midi_send_used(rt) + 2 < OZZY_MIDI_SEND_BUF_SIZE &&
	       snd_rawmidi_transmit_peek(rt->out, &byte, 1) == 1) {
		ozzy_midi_put_out(rt, byte);
		snd_rawmidi_transmit_ack(rt->out, 1);
	}
}

/*
 * ozzy_midi_consume - Read pending MIDI output bytes from the send ring.
 * Refills the ring from the rawmidi substream when it runs empty, and fills
 * with idle_byte when no data is pending. Called from device's fill_midi_out
 * in URB handler context (atomic).
 */
void ozzy_midi_consume(struct midi_runtime *rt, u8 *buffer, int count,
		       u8 idle_byte)
{
	unsigned long flags;
	int i;

	spin_lock_irqsave(&rt->out_lock, flags);
	for (i = 0; i < count; i++) {
		if (!ozzy_midi_send_used(rt))
			ozzy_midi_fill_send(rt);
		if (ozzy_midi_send_used(rt)) {
			buffer[i] = rt->send_buffer[rt->send_tail];
			rt->send_tail = (rt->send_tail + 1) & SEND_MASK;
		} else {
			buffer[i] = idle_byte;
		}
	}
	spin_unlock_irqrestore(&rt->out_lock, flags);
}

/* ========================================================================
 * URB Handlers
 * ======================================================================== */

/*
 * ozzy_midi_in_urb_handler - MIDI input URB completion handler.
 * Forwards received MIDI bytes to the ALSA rawmidi input substream.
 */
static void ozzy_midi_in_urb_handler(struct urb *usb_urb)
{
	struct midi_urb *in_urb = usb_urb->context;
	struct ozzy_chip *chip = in_urb->chip;
	struct midi_runtime *rt = chip->midi;
	const u8 *data = in_urb->buffer;
	unsigned int len = in_urb->instance.actual_length;
	u8 decoded[OZZY_MIDI_IN_MAX_BYTES];
	unsigned long flags;
	int ret;

	if (unlikely(ozzy_urb_gone(usb_urb->status)))
		goto in_fail;

	if (chip->ops->decode_midi_in) {
		len = chip->ops->decode_midi_in(chip, data, len, decoded);
		data = decoded;
	}

	spin_lock_irqsave(&rt->in_lock, flags);
	if (rt->in && len)
		snd_rawmidi_receive(rt->in, data, len);
	spin_unlock_irqrestore(&rt->in_lock, flags);

	ret = usb_submit_urb(&in_urb->instance, GFP_ATOMIC);
	if (ret < 0)
		goto in_fail;

	return;

in_fail:
	ozzy_midi_err(&in_urb->chip->dev->dev, "input URB failure\n");
}

/*
 * ozzy_midi_out_urb_handler - Dedicated MIDI OUT transfer done: the next timer
 * tick may send again.
 */
static void ozzy_midi_out_urb_handler(struct urb *usb_urb)
{
	struct midi_urb *out_urb = usb_urb->context;
	struct midi_runtime *rt = out_urb->chip->midi;

	if (unlikely(usb_urb->status && !ozzy_urb_gone(usb_urb->status)))
		ozzy_midi_err(&out_urb->chip->dev->dev, "output URB failure (%d)\n", usb_urb->status);

	clear_bit(0, &rt->out_busy);
}

/*
 * ozzy_midi_out_tick - Every info->midi_out_interval_us: send the next transfer
 * on the dedicated MIDI OUT endpoint if the previous one is done. One transfer at
 * a time and at most one per interval, so even an endpoint that never NAKs can't
 * send faster than the device's MIDI cable.
 */
static enum hrtimer_restart ozzy_midi_out_tick(struct hrtimer *timer)
{
	struct midi_runtime *rt = container_of(timer, struct midi_runtime, out_timer);
	struct ozzy_chip *chip = rt->chip;
	struct urb *urb = &rt->midi_out_urb.instance;

	if (!READ_ONCE(rt->out_ready))
		return HRTIMER_NORESTART;

	if (!test_and_set_bit(0, &rt->out_busy)) {
		urb->transfer_buffer_length = chip->ops->fill_midi_out_xfer(chip, rt->midi_out_urb.buffer);
		if (!urb->transfer_buffer_length || usb_submit_urb(urb, GFP_ATOMIC) < 0)
			clear_bit(0, &rt->out_busy);
	}

	hrtimer_forward_now(timer, rt->out_interval);
	return HRTIMER_RESTART;
}

/* ========================================================================
 * ALSA Rawmidi Operations
 * ======================================================================== */

static int ozzy_midi_in_open(struct snd_rawmidi_substream *alsa_sub)
{
	return 0;
}

static int ozzy_midi_in_close(struct snd_rawmidi_substream *alsa_sub)
{
	return 0;
}

static int ozzy_midi_out_open(struct snd_rawmidi_substream *alsa_sub)
{
	return 0;
}

static int ozzy_midi_out_close(struct snd_rawmidi_substream *alsa_sub)
{
	return 0;
}

/*
 * ozzy_midi_in_trigger - Enable or disable MIDI input.
 */
static void ozzy_midi_in_trigger(struct snd_rawmidi_substream *alsa_sub, int up)
{
	struct midi_runtime *rt = alsa_sub->rmidi->private_data;
	unsigned long flags;

	spin_lock_irqsave(&rt->in_lock, flags);
	if (up)
		rt->in = alsa_sub;
	else
		rt->in = NULL;
	spin_unlock_irqrestore(&rt->in_lock, flags);
}

/*
 * ozzy_midi_out_trigger - Enable or disable MIDI output.
 * While enabled, ozzy_midi_consume() keeps pulling from the substream, so
 * nothing is left behind when an application writes more than fits the ring.
 */
static void ozzy_midi_out_trigger(struct snd_rawmidi_substream *alsa_sub, int up)
{
	struct midi_runtime *rt = alsa_sub->rmidi->private_data;
	unsigned long flags;

	spin_lock_irqsave(&rt->out_lock, flags);
	if (up) {
		rt->out = alsa_sub;
		ozzy_midi_fill_send(rt);
	} else if (rt->out == alsa_sub) {
		rt->out = NULL;
	}
	spin_unlock_irqrestore(&rt->out_lock, flags);
}

static const struct snd_rawmidi_ops ozzy_midi_out_ops = {
	.open    = ozzy_midi_out_open,
	.close   = ozzy_midi_out_close,
	.trigger = ozzy_midi_out_trigger,
};

static const struct snd_rawmidi_ops ozzy_midi_in_ops = {
	.open    = ozzy_midi_in_open,
	.close   = ozzy_midi_in_close,
	.trigger = ozzy_midi_in_trigger,
};

/* ========================================================================
 * URB Initialization
 * ======================================================================== */

/*
 * ozzy_midi_init_bulk_in_urb - Initialize a single MIDI input URB.
 */
static int ozzy_midi_init_bulk_in_urb(struct midi_urb *urb,
				      struct ozzy_chip *chip, unsigned int ep)
{
	urb->chip = chip;
	usb_init_urb(&urb->instance);

	urb->buffer = kzalloc(OZZY_MIDI_PACKET_SIZE, GFP_KERNEL);
	if (!urb->buffer)
		return -ENOMEM;

	if (chip->dev->ep_in[ep] && usb_endpoint_xfer_int(&chip->dev->ep_in[ep]->desc))
		usb_fill_int_urb(&urb->instance, chip->dev,
				 usb_rcvintpipe(chip->dev, ep),
				 urb->buffer, 9,
				 ozzy_midi_in_urb_handler, urb,
				 chip->dev->ep_in[ep]->desc.bInterval);
	else
		usb_fill_bulk_urb(&urb->instance, chip->dev,
				  usb_rcvbulkpipe(chip->dev, ep),
				  urb->buffer, 9,
				  ozzy_midi_in_urb_handler, urb);

	if (usb_urb_ep_type_check(&urb->instance)) {
		ozzy_midi_err(&chip->dev->dev, "MIDI URB endpoint sanity check failed\n");
		return -EINVAL;
	}

	init_usb_anchor(&urb->submitted);
	return 0;
}

/*
 * ozzy_midi_init_out_urb - Initialize a dedicated MIDI OUT URB (bulk or interrupt,
 * as the endpoint says). Sent by ozzy_midi_out_tick().
 */
static int ozzy_midi_init_out_urb(struct midi_urb *urb, struct ozzy_chip *chip, unsigned int ep)
{
	struct usb_host_endpoint *hep = chip->dev->ep_out[ep];

	urb->chip = chip;
	usb_init_urb(&urb->instance);

	if (!urb->buffer)
		urb->buffer = kzalloc(OZZY_MIDI_OUT_XFER_MAX, GFP_KERNEL);
	if (!urb->buffer)
		return -ENOMEM;

	if (hep && usb_endpoint_xfer_int(&hep->desc))
		usb_fill_int_urb(&urb->instance, chip->dev, usb_sndintpipe(chip->dev, ep),
				 urb->buffer, OZZY_MIDI_OUT_XFER_MAX,
				 ozzy_midi_out_urb_handler, urb, hep->desc.bInterval);
	else
		usb_fill_bulk_urb(&urb->instance, chip->dev, usb_sndbulkpipe(chip->dev, ep),
				  urb->buffer, OZZY_MIDI_OUT_XFER_MAX,
				  ozzy_midi_out_urb_handler, urb);

	if (usb_urb_ep_type_check(&urb->instance)) {
		ozzy_midi_err(&chip->dev->dev, "MIDI out URB endpoint sanity check failed\n");
		return -EINVAL;
	}
	return 0;
}

/*
 * ozzy_midi_init_urbs - Initialize and submit all MIDI input URBs.
 * Called during initial probe and after USB device reset (post_reset).
 */
int ozzy_midi_init_urbs(struct ozzy_chip *chip)
{
	struct midi_runtime *rt = chip->midi;
	uint8_t i;
	int ret;

	if (!rt)
		return -EINVAL;

	rt->chip = chip;

	for (i = 0; i < OZZY_MIDI_N_URBS && chip->midi_in_ep; i++) {
		ret = ozzy_midi_init_bulk_in_urb(&rt->midi_in_urbs[i], chip,
						 chip->midi_in_ep);
		if (ret < 0)
			goto error;
	}

	for (i = 0; i < OZZY_MIDI_N_URBS && chip->midi_in_ep; i++) {
		usb_anchor_urb(&rt->midi_in_urbs[i].instance,
			       &rt->midi_in_urbs[i].submitted);
		ret = usb_submit_urb(&rt->midi_in_urbs[i].instance, GFP_ATOMIC);
		if (ret < 0) {
			ozzy_midi_abort(chip);
			goto error;
		}
	}

	/* Dedicated MIDI OUT: sent by ozzy_midi_out_tick() */
	if (rt->has_out_ep) {
		ret = ozzy_midi_init_out_urb(&rt->midi_out_urb, chip, chip->midi_out_ep);
		if (ret < 0) {
			ozzy_midi_abort(chip);
			goto error;
		}
		rt->out_busy = 0;
		WRITE_ONCE(rt->out_ready, true);
		hrtimer_start(&rt->out_timer, rt->out_interval, HRTIMER_MODE_REL_SOFT);
	}

	return 0;

error:
	for (i = 0; i < OZZY_MIDI_N_URBS; i++)
		kfree(rt->midi_in_urbs[i].buffer);
	return ret;
}

/*
 * ozzy_midi_kill_urbs - Kill all MIDI URBs.
 */
static void ozzy_midi_kill_urbs(struct midi_runtime *rt)
{
	int i, time;

	/* The in URBs (and their anchors) only exist with a MIDI IN endpoint */
	for (i = 0; i < OZZY_MIDI_N_URBS && rt->chip->midi_in_ep; i++) {
		time = usb_wait_anchor_empty_timeout(
			&rt->midi_in_urbs[i].submitted, 100);
		if (!time)
			usb_kill_anchored_urbs(&rt->midi_in_urbs[i].submitted);
		usb_kill_urb(&rt->midi_in_urbs[i].instance);
	}

	/* Stop the MIDI OUT timer first, then wait for the transfer in flight */
	if (rt->has_out_ep) {
		WRITE_ONCE(rt->out_ready, false);
		hrtimer_cancel(&rt->out_timer);
		usb_kill_urb(&rt->midi_out_urb.instance);
	}
}

/*
 * ozzy_midi_abort - Stop all MIDI URBs immediately.
 */
void ozzy_midi_abort(struct ozzy_chip *chip)
{
	struct midi_runtime *rt = chip->midi;

	if (rt && rt->active)
		ozzy_midi_kill_urbs(rt);
}

/* ========================================================================
 * Init
 * ======================================================================== */

/*
 * ozzy_midi_init - Create and initialize the MIDI subsystem.
 * Allocates the runtime, registers ALSA rawmidi, and sets up URBs.
 */
int ozzy_midi_init(struct ozzy_chip *chip)
{
	struct snd_rawmidi *midi;
	struct midi_runtime *rt;
	int ret;

	rt = kzalloc(sizeof(struct midi_runtime), GFP_KERNEL);
	if (!rt)
		return -ENOMEM;

	rt->send_buffer = kzalloc(OZZY_MIDI_SEND_BUF_SIZE, GFP_KERNEL);
	if (!rt->send_buffer) {
		kfree(rt);
		return -ENOMEM;
	}

	rt->chip = chip;
	rt->send_head = 0;
	rt->send_tail = 0;
	rt->run_status = 0;
	rt->run_remaining = 0;

	spin_lock_init(&rt->in_lock);
	spin_lock_init(&rt->out_lock);

	rt->has_out_ep = chip->midi_out_ep && chip->ops->fill_midi_out_xfer;
	if (rt->has_out_ep) {
		rt->out_interval = ns_to_ktime((u64)chip->info->midi_out_interval_us * NSEC_PER_USEC);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
		hrtimer_setup(&rt->out_timer, ozzy_midi_out_tick, CLOCK_MONOTONIC, HRTIMER_MODE_REL_SOFT);
#else
		hrtimer_init(&rt->out_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_SOFT);
		rt->out_timer.function = ozzy_midi_out_tick;
#endif
	}

	ret = snd_rawmidi_new(chip->card, chip->dev->product, 0, 1, 1, &midi);
	if (ret < 0) {
		ozzy_midi_err(&chip->dev->dev, "Cannot create MIDI instance\n");
		kfree(rt->send_buffer);
		kfree(rt);
		return ret;
	}

	midi->private_data = rt;

	strscpy(midi->name, chip->dev->product, sizeof(midi->name));
	midi->info_flags = SNDRV_RAWMIDI_INFO_OUTPUT |
			   SNDRV_RAWMIDI_INFO_INPUT |
			   SNDRV_RAWMIDI_INFO_DUPLEX;
	snd_rawmidi_set_ops(midi, SNDRV_RAWMIDI_STREAM_OUTPUT, &ozzy_midi_out_ops);
	snd_rawmidi_set_ops(midi, SNDRV_RAWMIDI_STREAM_INPUT, &ozzy_midi_in_ops);

	rt->instance = midi;
	chip->midi = rt;
	rt->active = true;

	/* Set up and submit MIDI URBs */
	if (chip->midi_in_ep || rt->has_out_ep) {
		ret = ozzy_midi_init_urbs(chip);
		if (ret < 0) {
			ozzy_midi_err(&chip->dev->dev, "MIDI URB setup failed\n");
			kfree(rt->send_buffer);
			kfree(rt);
			return ret;
		}
	}

	return 0;
}
