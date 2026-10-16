// SPDX-License-Identifier: MIT
/*
 * Ozzy USB Audio Driver - PCM Subsystem
 *
 * Device-agnostic ALSA PCM implementation. Handles URB lifecycle,
 * ALSA ops, and DMA ring buffer management. Actual audio encoding/
 * decoding is delegated to device ops (process_out_packet / process_in_packet).
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include <linux/delay.h>
#include <linux/slab.h>
#include <sound/pcm.h>

#include "ozzy.h"
#include "ozzy_log.h"
#include "ozzy_pcm.h"

/* ========================================================================
 * Stream State Management
 * ======================================================================== */

/*
 * ozzy_pcm_get_substream - Map an ALSA substream to our internal struct.
 */
static struct pcm_substream *ozzy_pcm_get_substream(struct snd_pcm_substream *alsa_sub)
{
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);

	if (alsa_sub->stream == SNDRV_PCM_STREAM_PLAYBACK)
		return &rt->playback;
	else if (alsa_sub->stream == SNDRV_PCM_STREAM_CAPTURE)
		return &rt->capture;

	ozzy_pcm_err(&rt->chip->dev->dev, "Invalid PCM stream type\n");
	return NULL;
}

/*
 * ozzy_pcm_stream_stop - Transition stream to disabled state.
 */
static void ozzy_pcm_stream_stop(struct pcm_runtime *rt)
{
	if (rt->stream_state != STREAM_DISABLED) {
		rt->stream_state = STREAM_STOPPING;
		rt->stream_state = STREAM_DISABLED;
	}
}

/*
 * ozzy_pcm_stream_start - Transition stream to running state.
 * Call with stream_mutex held.
 */
static int ozzy_pcm_stream_start(struct pcm_runtime *rt)
{
	if (rt->stream_state == STREAM_DISABLED) {
		rt->panic = false;
		rt->stream_state = STREAM_STARTING;
		rt->stream_state = STREAM_RUNNING;
	}
	return 0;
}

/* ========================================================================
 * URB Lifecycle
 * ======================================================================== */

/*
 * ozzy_pcm_kill_urbs - Kill all PCM URBs (waits for completion).
 */
static void ozzy_pcm_kill_urbs(struct pcm_runtime *rt)
{
	int i, time;

	for (i = 0; i < OZZY_PCM_N_URBS; i++) {
		time = usb_wait_anchor_empty_timeout(&rt->pcm_in_urbs[i].submitted, 100);
		if (!time)
			usb_kill_anchored_urbs(&rt->pcm_in_urbs[i].submitted);
		time = usb_wait_anchor_empty_timeout(&rt->pcm_out_urbs[i].submitted, 100);
		if (!time)
			usb_kill_anchored_urbs(&rt->pcm_out_urbs[i].submitted);
		usb_kill_urb(&rt->pcm_in_urbs[i].instance);
		usb_kill_urb(&rt->pcm_out_urbs[i].instance);
	}
	for (i = 0; i < OZZY_PCM_N_ISO_URBS; i++)
		usb_kill_urb(rt->pcm_iso_out_urbs[i].instance);
	for (i = 0; i < OZZY_PCM_N_FB_URBS; i++)
		usb_kill_urb(rt->pcm_fb_urbs[i].instance);
}

/*
 * ozzy_pcm_poison_urbs - Poison all PCM URBs (prevents resubmission).
 */
static void ozzy_pcm_poison_urbs(struct pcm_runtime *rt)
{
	int i, time;

	for (i = 0; i < OZZY_PCM_N_URBS; i++) {
		time = usb_wait_anchor_empty_timeout(&rt->pcm_in_urbs[i].submitted, 100);
		if (!time)
			usb_kill_anchored_urbs(&rt->pcm_in_urbs[i].submitted);
		time = usb_wait_anchor_empty_timeout(&rt->pcm_out_urbs[i].submitted, 100);
		if (!time)
			usb_kill_anchored_urbs(&rt->pcm_out_urbs[i].submitted);
		usb_poison_urb(&rt->pcm_in_urbs[i].instance);
		usb_poison_urb(&rt->pcm_out_urbs[i].instance);
	}
	for (i = 0; i < OZZY_PCM_N_ISO_URBS; i++)
		usb_poison_urb(rt->pcm_iso_out_urbs[i].instance);
	for (i = 0; i < OZZY_PCM_N_FB_URBS; i++)
		usb_poison_urb(rt->pcm_fb_urbs[i].instance);
}

/* ========================================================================
 * URB Completion Handlers
 * ======================================================================== */

/*
 * ozzy_pcm_in_urb_handler - Input URB completion handler.
 * Calls the device's process_in_packet to decode audio into the ALSA DMA area.
 */
static void ozzy_pcm_in_urb_handler(struct urb *usb_urb)
{
	struct pcm_urb *in_urb = usb_urb->context;
	struct ozzy_chip *chip = in_urb->chip;
	struct pcm_runtime *rt = chip->pcm;
	struct pcm_substream *sub;
	bool do_period_elapsed = false;
	unsigned long flags;
	unsigned int bytes;
	int ret;

	if (rt->panic || rt->stream_state == STREAM_STOPPING)
		return;

	if (unlikely(ozzy_urb_gone(usb_urb->status)))
		goto in_fail;

	sub = &rt->capture;
	spin_lock_irqsave(&sub->lock, flags);
	if (sub->active) {
		struct snd_pcm_runtime *alsa_rt = sub->instance->runtime;
		unsigned int pcm_buffer_size = snd_pcm_lib_buffer_bytes(sub->instance);

		bytes = chip->ops->process_in_packet(chip, in_urb->buffer,
						     alsa_rt->dma_area,
						     sub->dma_off,
						     pcm_buffer_size);

		sub->dma_off += bytes;
		if (sub->dma_off >= pcm_buffer_size)
			sub->dma_off -= pcm_buffer_size;

		sub->period_off += bytes;
		if (sub->period_off >= alsa_rt->period_size) {
			sub->period_off %= alsa_rt->period_size;
			do_period_elapsed = true;
		}
	}
	spin_unlock_irqrestore(&sub->lock, flags);

	if (do_period_elapsed)
		snd_pcm_period_elapsed(sub->instance);

	ret = usb_submit_urb(&in_urb->instance, GFP_ATOMIC);
	if (ret < 0)
		goto in_fail;

	return;

in_fail:
	ozzy_pcm_err(&chip->dev->dev, "PCM input URB failure\n");
	rt->panic = true;
}

/*
 * ozzy_pcm_out_urb_handler - Output URB completion handler.
 * Unified handler for both bulk and interrupt output. Calls device's
 * process_out_packet for audio and fill_midi_out for embedded MIDI.
 */
static void ozzy_pcm_out_urb_handler(struct urb *usb_urb)
{
	struct pcm_urb *out_urb = usb_urb->context;
	struct ozzy_chip *chip = out_urb->chip;
	struct pcm_runtime *rt = chip->pcm;
	struct pcm_substream *sub;
	bool do_period_elapsed = false;
	unsigned long flags;
	unsigned int bytes;
	int ret;

	if (rt->panic || rt->stream_state == STREAM_STOPPING)
		return;

	if (unlikely(ozzy_urb_gone(usb_urb->status)))
		goto out_fail;

	sub = &rt->playback;
	spin_lock_irqsave(&sub->lock, flags);
	if (sub->active) {
		struct snd_pcm_runtime *alsa_rt = sub->instance->runtime;
		unsigned int pcm_buffer_size = snd_pcm_lib_buffer_bytes(sub->instance);

		bytes = chip->ops->process_out_packet(chip, out_urb->buffer,
						      alsa_rt->dma_area,
						      sub->dma_off,
						      pcm_buffer_size);

		sub->dma_off += bytes;
		if (sub->dma_off >= pcm_buffer_size)
			sub->dma_off -= pcm_buffer_size;

		sub->period_off += bytes;
		if (sub->period_off >= alsa_rt->period_size) {
			sub->period_off %= alsa_rt->period_size;
			do_period_elapsed = true;
		}
	} else {
		/* No active playback -- re-initialize silence pattern */
		chip->ops->init_out_urb(chip, out_urb->buffer);
	}
	spin_unlock_irqrestore(&sub->lock, flags);

	if (do_period_elapsed)
		snd_pcm_period_elapsed(sub->instance);

	/* Embed pending MIDI output bytes */
	if (chip->ops->fill_midi_out)
		chip->ops->fill_midi_out(chip, out_urb->buffer);

	ret = usb_submit_urb(&out_urb->instance, GFP_ATOMIC);
	if (ret < 0)
		goto out_fail;

	return;

out_fail:
	ozzy_pcm_err(&chip->dev->dev, "PCM output URB failure\n");
	rt->panic = true;
}

/*
 * ozzy_pcm_fill_iso_out - Lay out an isochronous OUT URB: one packet per service
 * interval, each with as many frames as the feedback asks for, filled from the
 * ALSA ring at @dma_off (the device takes ALSA's samples as they are), or with
 * silence when @dma_area is NULL. Returns the ALSA bytes consumed.
 */
static unsigned int ozzy_pcm_fill_iso_out(struct pcm_runtime *rt, struct urb *urb,
					  const uint8_t *dma_area, unsigned int dma_off,
					  unsigned int pcm_buffer_size)
{
	uint8_t *buf = urb->transfer_buffer;
	unsigned int p, bytes, first, off = 0;

	for (p = 0; p < urb->number_of_packets; p++) {
		bytes = min(isoc_fb_out_frames(&rt->fb, rt->iso_uframes), rt->iso_max_frames) *
			rt->iso_frame_bytes;
		urb->iso_frame_desc[p].offset = off;
		urb->iso_frame_desc[p].length = bytes;
		off += bytes;
	}
	urb->transfer_buffer_length = off;

	if (!dma_area) {
		memset(buf, 0, off);
		return 0;
	}
	first = min(off, pcm_buffer_size - dma_off);
	memcpy(buf, dma_area + dma_off, first);
	memcpy(buf + first, dma_area, off - first);
	return off;
}

/*
 * ozzy_pcm_iso_out_urb_handler - Isochronous output URB completion handler.
 */
static void ozzy_pcm_iso_out_urb_handler(struct urb *usb_urb)
{
	struct pcm_iso_urb *out_urb = usb_urb->context;
	struct ozzy_chip *chip = out_urb->chip;
	struct pcm_runtime *rt = chip->pcm;
	struct pcm_substream *sub;
	bool do_period_elapsed = false;
	unsigned long flags;
	unsigned int bytes;
	int ret;

	if (rt->panic || rt->stream_state == STREAM_STOPPING)
		return;

	if (unlikely(ozzy_urb_gone(usb_urb->status)))
		goto out_fail;

	sub = &rt->playback;
	spin_lock_irqsave(&sub->lock, flags);
	if (sub->active) {
		struct snd_pcm_runtime *alsa_rt = sub->instance->runtime;
		unsigned int pcm_buffer_size = snd_pcm_lib_buffer_bytes(sub->instance);

		bytes = ozzy_pcm_fill_iso_out(rt, usb_urb, alsa_rt->dma_area,
					      sub->dma_off, pcm_buffer_size);

		sub->dma_off += bytes;
		if (sub->dma_off >= pcm_buffer_size)
			sub->dma_off -= pcm_buffer_size;

		sub->period_off += bytes;
		if (sub->period_off >= alsa_rt->period_size) {
			sub->period_off %= alsa_rt->period_size;
			do_period_elapsed = true;
		}
	} else {
		ozzy_pcm_fill_iso_out(rt, usb_urb, NULL, 0, 0);
	}
	spin_unlock_irqrestore(&sub->lock, flags);

	if (do_period_elapsed)
		snd_pcm_period_elapsed(sub->instance);

	ret = usb_submit_urb(usb_urb, GFP_ATOMIC);
	if (ret < 0)
		goto out_fail;

	return;

out_fail:
	ozzy_pcm_err(&chip->dev->dev, "PCM isochronous output URB failure\n");
	rt->panic = true;
}

/*
 * ozzy_pcm_fb_urb_handler - Isochronous feedback URB completion handler:
 * every packet goes into the output pacing ring.
 */
static void ozzy_pcm_fb_urb_handler(struct urb *usb_urb)
{
	struct pcm_iso_urb *fb_urb = usb_urb->context;
	struct ozzy_chip *chip = fb_urb->chip;
	struct pcm_runtime *rt = chip->pcm;
	const struct usb_iso_packet_descriptor *d;
	unsigned int p, len;
	int ret;

	if (rt->panic || rt->stream_state == STREAM_STOPPING)
		return;

	if (unlikely(ozzy_urb_gone(usb_urb->status)))
		return;

	for (p = 0; p < usb_urb->number_of_packets; p++) {
		d = &usb_urb->iso_frame_desc[p];
		len = d->status ? 0 : d->actual_length;
		if (unlikely(!rt->fb_logged)) {
			rt->fb_logged = true;
			ozzy_pcm_log(&chip->dev->dev, "first feedback packet: %u bytes %*ph\n",
				     len, (int)min(len, 8u), fb_urb->buffer + d->offset);
		}
		if (isoc_fb_packet(&rt->fb, fb_urb->buffer + d->offset, len))
			ozzy_pcm_err(&chip->dev->dev,
				     "more than %u feedback packets were not %u bytes long, PCM out runs at the nominal rate\n",
				     rt->fb.cfg.max_errors, rt->fb.cfg.packet_size);
	}

	ret = usb_submit_urb(usb_urb, GFP_ATOMIC);
	if (ret < 0)
		ozzy_pcm_err(&chip->dev->dev, "PCM feedback URB failure (%d)\n", ret);
}

/* ========================================================================
 * ALSA PCM Operations
 * ======================================================================== */

/*
 * ozzy_pcm_open - ALSA PCM open callback.
 * Builds snd_pcm_hardware dynamically from the device's info descriptor.
 */
static int ozzy_pcm_open(struct snd_pcm_substream *alsa_sub)
{
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);
	struct ozzy_chip *chip = rt->chip;
	const struct ozzy_device_info *info = chip->info;
	struct pcm_substream *sub;
	struct snd_pcm_runtime *alsa_rt = alsa_sub->runtime;
	bool playback = alsa_sub->stream == SNDRV_PCM_STREAM_PLAYBACK;
	unsigned int channels = playback ? info->playback_channels : info->capture_channels;
	/* Isochronous out: one URB at the highest rate */
	unsigned int frames_per_packet = !playback ? info->frames_per_in_packet :
		info->out_isoc ? info->rate_max * OZZY_PCM_ISO_UFRAMES / ISOC_FB_UFRAMES_PER_SEC :
		info->frames_per_out_packet;
	unsigned int alsa_pkt_bytes = frames_per_packet * channels * info->bytes_per_sample;

	if (rt->panic)
		return -EPIPE;

	mutex_lock(&rt->stream_mutex);

	/* Build hardware descriptor from device info */
	alsa_rt->hw.info = SNDRV_PCM_INFO_MMAP |
			   SNDRV_PCM_INFO_INTERLEAVED |
			   SNDRV_PCM_INFO_BLOCK_TRANSFER |
			   SNDRV_PCM_INFO_PAUSE |
			   SNDRV_PCM_INFO_MMAP_VALID;
	alsa_rt->hw.formats = info->alsa_format;
	alsa_rt->hw.rates = info->rates_mask;
	alsa_rt->hw.rate_min = info->rate_min;
	alsa_rt->hw.rate_max = info->rate_max;
	alsa_rt->hw.channels_min = channels;
	alsa_rt->hw.channels_max = channels;
	alsa_rt->hw.buffer_bytes_max = 2000 * alsa_pkt_bytes;
	alsa_rt->hw.period_bytes_min = 2 * alsa_pkt_bytes;
	alsa_rt->hw.period_bytes_max = 2000 * alsa_pkt_bytes;
	alsa_rt->hw.periods_min = 2;
	alsa_rt->hw.periods_max = 1024;

	if (alsa_sub->stream == SNDRV_PCM_STREAM_PLAYBACK)
		sub = &rt->playback;
	else if (alsa_sub->stream == SNDRV_PCM_STREAM_CAPTURE)
		sub = &rt->capture;
	else {
		mutex_unlock(&rt->stream_mutex);
		return -EINVAL;
	}

	sub->instance = alsa_sub;
	sub->active = false;
	mutex_unlock(&rt->stream_mutex);
	return 0;
}

/*
 * ozzy_pcm_close - ALSA PCM close callback.
 * Deactivates the substream and stops streaming if all substreams are closed.
 */
static int ozzy_pcm_close(struct snd_pcm_substream *alsa_sub)
{
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);
	struct ozzy_chip *chip = rt->chip;
	struct pcm_substream *sub = ozzy_pcm_get_substream(alsa_sub);
	unsigned long flags;

	if (rt->panic)
		return 0;

	mutex_lock(&rt->stream_mutex);
	if (sub) {
		spin_lock_irqsave(&sub->lock, flags);
		sub->instance = NULL;
		sub->active = false;
		spin_unlock_irqrestore(&sub->lock, flags);

		if (!rt->playback.instance && !rt->capture.instance) {
			ozzy_pcm_stream_stop(rt);
			rt->rate = chip->info->num_rates;
		}
	}
	mutex_unlock(&rt->stream_mutex);
	return 0;
}

/*
 * ozzy_pcm_set_rate - Handle sample rate changes, resetting device if needed.
 * Call with stream_mutex held.
 */
static int ozzy_pcm_set_rate(struct pcm_runtime *rt)
{
	struct ozzy_chip *chip = rt->chip;

	chip->requested_rate = rt->rate;

	if (chip->requested_rate != chip->current_rate) {
		ozzy_pcm_err(&chip->dev->dev,
			   "Resetting device for sample rate change %u -> %u\n",
			   chip->info->rates[chip->current_rate],
			   chip->info->rates[chip->requested_rate]);

		/*
		 * Reset the device -- pre_reset/post_reset handle URB lifecycle.
		 * The post-reset handshake sets requested_rate; the device does not
		 * answer SET_CUR while streaming, so it is not sent here.
		 */
		mutex_unlock(&rt->stream_mutex);
		chip->ops->reset(chip);
		mutex_lock(&rt->stream_mutex);
	}

	return 0;
}

/*
 * ozzy_pcm_prepare - ALSA PCM prepare callback.
 * Validates format, sets sample rate, and starts the stream.
 */
static int ozzy_pcm_prepare(struct snd_pcm_substream *alsa_sub)
{
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);
	struct ozzy_chip *chip = rt->chip;
	struct pcm_substream *sub = ozzy_pcm_get_substream(alsa_sub);
	struct snd_pcm_runtime *alsa_rt = alsa_sub->runtime;
	unsigned int i;
	int ret;

	if (rt->panic)
		return -EPIPE;
	if (!sub)
		return -ENODEV;

	mutex_lock(&rt->stream_mutex);

	if (alsa_rt->format != SNDRV_PCM_FORMAT_S24_3LE) {
		mutex_unlock(&rt->stream_mutex);
		return -EINVAL;
	}

	sub->dma_off = 0;
	sub->period_off = 0;

	if (rt->stream_state == STREAM_DISABLED) {
		/* Find the rate index */
		for (i = 0; i < chip->info->num_rates; i++) {
			if (alsa_rt->rate == chip->info->rates[i])
				break;
		}
		if (i == chip->info->num_rates) {
			mutex_unlock(&rt->stream_mutex);
			ozzy_pcm_err(&chip->dev->dev, "Invalid sample rate %d\n",
				alsa_rt->rate);
			return -EINVAL;
		}

		rt->rate = i;

		ret = ozzy_pcm_set_rate(rt);
		if (ret) {
			mutex_unlock(&rt->stream_mutex);
			return ret;
		}

		ret = ozzy_pcm_stream_start(rt);
		if (ret) {
			mutex_unlock(&rt->stream_mutex);
			ozzy_pcm_err(&chip->dev->dev, "Could not start PCM stream\n");
			return ret;
		}
	}

	mutex_unlock(&rt->stream_mutex);
	return 0;
}

/*
 * ozzy_pcm_trigger - ALSA PCM trigger callback.
 * Activates or deactivates the substream for URB processing.
 */
static int ozzy_pcm_trigger(struct snd_pcm_substream *alsa_sub, int cmd)
{
	struct pcm_substream *sub = ozzy_pcm_get_substream(alsa_sub);
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);

	if (rt->panic)
		return -EPIPE;
	if (!sub)
		return -ENODEV;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		spin_lock_irq(&sub->lock);
		sub->active = true;
		spin_unlock_irq(&sub->lock);
		return 0;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		spin_lock_irq(&sub->lock);
		sub->active = false;
		spin_unlock_irq(&sub->lock);
		return 0;

	default:
		return -EINVAL;
	}
}

/*
 * ozzy_pcm_pointer - ALSA PCM pointer callback.
 * Returns the current DMA position in frames.
 */
static snd_pcm_uframes_t ozzy_pcm_pointer(struct snd_pcm_substream *alsa_sub)
{
	struct pcm_substream *sub = ozzy_pcm_get_substream(alsa_sub);
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);
	unsigned long flags;
	snd_pcm_uframes_t dma_offset;

	if (rt->panic || !sub) {
		ozzy_pcm_err(&rt->chip->dev->dev, "PCM XRUN\n");
		return SNDRV_PCM_POS_XRUN;
	}

	spin_lock_irqsave(&sub->lock, flags);
	dma_offset = sub->dma_off;
	spin_unlock_irqrestore(&sub->lock, flags);

	return bytes_to_frames(alsa_sub->runtime, dma_offset);
}

static const struct snd_pcm_ops ozzy_pcm_ops = {
	.open    = ozzy_pcm_open,
	.close   = ozzy_pcm_close,
	.prepare = ozzy_pcm_prepare,
	.trigger = ozzy_pcm_trigger,
	.pointer = ozzy_pcm_pointer,
};

/* ========================================================================
 * URB Initialization
 * ======================================================================== */

/*
 * ozzy_pcm_init_out_urb - Initialize a single output URB.
 * Detects bulk vs interrupt and fills the appropriate USB pipe.
 */
static int ozzy_pcm_init_out_urb(struct pcm_urb *urb, struct ozzy_chip *chip)
{
	const struct ozzy_device_info *info = chip->info;
	unsigned int pkt_size;

	urb->chip = chip;
	usb_init_urb(&urb->instance);

	/* Get packet size (may differ for bulk vs interrupt) */
	if (chip->ops->get_out_packet_size)
		pkt_size = chip->ops->get_out_packet_size(chip, chip->is_bulk);
	else
		pkt_size = info->out_packet_size;

	urb->buffer = kzalloc(pkt_size, GFP_KERNEL);
	if (!urb->buffer)
		return -ENOMEM;

	/* Fill initial silence pattern */
	if (chip->ops->init_out_urb)
		chip->ops->init_out_urb(chip, urb->buffer);

	if (chip->is_bulk) {
		usb_fill_bulk_urb(&urb->instance, chip->dev,
				  usb_sndbulkpipe(chip->dev, chip->out_ep),
				  urb->buffer, pkt_size,
				  ozzy_pcm_out_urb_handler, urb);
	} else {
		usb_fill_int_urb(&urb->instance, chip->dev,
				 usb_sndintpipe(chip->dev, chip->out_ep),
				 urb->buffer, pkt_size,
				 ozzy_pcm_out_urb_handler, urb,
				 chip->dev->ep_out[chip->out_ep]->desc.bInterval);
	}

	if (usb_urb_ep_type_check(&urb->instance)) {
		ozzy_pcm_err(&chip->dev->dev, "Output URB endpoint sanity check failed\n");
		return -EINVAL;
	}

	init_usb_anchor(&urb->submitted);
	return 0;
}

/*
 * ozzy_pcm_init_in_urb - Initialize a single input URB.
 * Detects bulk vs interrupt and fills the appropriate USB pipe.
 */
static int ozzy_pcm_init_in_urb(struct pcm_urb *urb, struct ozzy_chip *chip)
{
	const struct ozzy_device_info *info = chip->info;

	urb->chip = chip;
	usb_init_urb(&urb->instance);

	urb->buffer = kzalloc(info->in_packet_size, GFP_KERNEL);
	if (!urb->buffer)
		return -ENOMEM;

	if ((chip->dev->ep_in[chip->in_ep]->desc.bmAttributes &
	     USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_BULK) {
		usb_fill_bulk_urb(&urb->instance, chip->dev,
				  usb_rcvbulkpipe(chip->dev, chip->in_ep),
				  urb->buffer, info->in_packet_size,
				  ozzy_pcm_in_urb_handler, urb);
	} else {
		usb_fill_int_urb(&urb->instance, chip->dev,
				 usb_rcvintpipe(chip->dev, chip->in_ep),
				 urb->buffer, info->in_packet_size,
				 ozzy_pcm_in_urb_handler, urb,
				 chip->dev->ep_in[chip->in_ep]->desc.bInterval);
	}

	if (usb_urb_ep_type_check(&urb->instance)) {
		ozzy_pcm_err(&chip->dev->dev, "Input URB endpoint sanity check failed\n");
		return -EINVAL;
	}

	init_usb_anchor(&urb->submitted);
	return 0;
}

/*
 * ozzy_pcm_iso_packets - Packets per isochronous URB on @epd: OZZY_PCM_ISO_UFRAMES
 * microframes, one packet per service interval (2^(bInterval-1) microframes).
 */
static unsigned int ozzy_pcm_iso_packets(const struct usb_endpoint_descriptor *epd)
{
	return max(OZZY_PCM_ISO_UFRAMES / isoc_fb_interval_uframes(epd->bInterval), 1u);
}

/*
 * ozzy_pcm_init_iso_urb - Set up an isochronous URB on @ep: allocate it the first
 * time, unpoison it after a reset. Every packet gets one wMaxPacketSize (the
 * feedback layout); ozzy_pcm_fill_iso_out() lays out the OUT ones.
 */
static int ozzy_pcm_init_iso_urb(struct pcm_iso_urb *u, struct ozzy_chip *chip,
				 struct usb_host_endpoint *ep, usb_complete_t complete)
{
	const struct usb_endpoint_descriptor *epd = &ep->desc;
	unsigned int packets = ozzy_pcm_iso_packets(epd);
	unsigned int maxp = usb_endpoint_maxp(epd) * usb_endpoint_maxp_mult(epd);
	unsigned int num = usb_endpoint_num(epd);
	struct urb *urb;
	unsigned int i;

	if (!u->instance) {
		u->instance = usb_alloc_urb(packets, GFP_KERNEL);
		u->buffer = kzalloc(packets * maxp, GFP_KERNEL);
		if (!u->instance || !u->buffer)
			return -ENOMEM;
	} else {
		usb_unpoison_urb(u->instance);
	}

	u->chip = chip;
	urb = u->instance;
	urb->dev = chip->dev;
	urb->pipe = usb_endpoint_dir_in(epd) ? usb_rcvisocpipe(chip->dev, num)
					     : usb_sndisocpipe(chip->dev, num);
	urb->transfer_flags = URB_ISO_ASAP;
	urb->transfer_buffer = u->buffer;
	urb->transfer_buffer_length = packets * maxp;
	urb->number_of_packets = packets;
	urb->interval = isoc_fb_interval_uframes(epd->bInterval);
	urb->complete = complete;
	urb->context = u;
	for (i = 0; i < packets; i++) {
		urb->iso_frame_desc[i].offset = i * maxp;
		urb->iso_frame_desc[i].length = maxp;
	}

	if (usb_urb_ep_type_check(urb)) {
		ozzy_pcm_err(&chip->dev->dev, "Isochronous URB endpoint sanity check failed\n");
		return -EINVAL;
	}
	return 0;
}

/*
 * ozzy_pcm_init_iso_urbs - Set up isochronous output at the current rate: the
 * pacing (from the feedback endpoint if there is one), the OUT URBs (filled with
 * silence) and the feedback URBs.
 */
static int ozzy_pcm_init_iso_urbs(struct ozzy_chip *chip)
{
	struct pcm_runtime *rt = chip->pcm;
	const struct ozzy_device_info *info = chip->info;
	const struct usb_endpoint_descriptor *out = &chip->dev->ep_out[chip->out_ep]->desc;
	struct usb_host_endpoint *fb_ep = chip->feedback_ep ? chip->dev->ep_in[chip->feedback_ep] : NULL;
	static const struct isoc_fb_config no_feedback = { ISOC_FB_NONE, 0, 0 };
	unsigned int rate = info->rates[chip->current_rate];
	unsigned int i;
	int ret;

	rt->iso_frame_bytes = info->playback_channels * info->bytes_per_sample;
	rt->iso_uframes = isoc_fb_interval_uframes(out->bInterval);
	rt->iso_max_frames = usb_endpoint_maxp(out) * usb_endpoint_maxp_mult(out) / rt->iso_frame_bytes;
	if (rt->iso_max_frames < isoc_fb_max_frames(rate, rt->iso_uframes))
		ozzy_pcm_err(&chip->dev->dev, "PCM out wMaxPacketSize is too small for %u Hz\n", rate);

	if (!isoc_fb_init(&rt->fb, rate, fb_ep ? &info->feedback : &no_feedback,
			  fb_ep ? fb_ep->desc.bInterval : 0))
		ozzy_pcm_err(&chip->dev->dev, "feedback bInterval %u not supported, PCM out runs at the nominal rate\n",
			     fb_ep->desc.bInterval);
	rt->fb_logged = false;
	ozzy_pcm_log(&chip->dev->dev, "isochronous out: %u microframe(s) per packet, up to %u frames, feedback every %u microframe(s)\n",
		     rt->iso_uframes, rt->iso_max_frames, rt->fb.uframes);

	for (i = 0; i < OZZY_PCM_N_ISO_URBS; i++) {
		ret = ozzy_pcm_init_iso_urb(&rt->pcm_iso_out_urbs[i], chip,
					    chip->dev->ep_out[chip->out_ep],
					    ozzy_pcm_iso_out_urb_handler);
		if (ret < 0)
			return ret;
		ozzy_pcm_fill_iso_out(rt, rt->pcm_iso_out_urbs[i].instance, NULL, 0, 0);
	}

	for (i = 0; fb_ep && i < OZZY_PCM_N_FB_URBS; i++) {
		ret = ozzy_pcm_init_iso_urb(&rt->pcm_fb_urbs[i], chip, fb_ep,
					    ozzy_pcm_fb_urb_handler);
		if (ret < 0)
			return ret;
	}
	return 0;
}

/*
 * ozzy_pcm_init_urbs - Initialize and submit all PCM URBs.
 * Called during initial probe and after USB device reset (post_reset).
 */
int ozzy_pcm_init_urbs(struct ozzy_chip *chip)
{
	struct pcm_runtime *rt = chip->pcm;
	uint8_t i;
	int ret;

	rt->chip = chip;

	/* Initialize input URBs */
	for (i = 0; i < OZZY_PCM_N_URBS; i++) {
		ret = ozzy_pcm_init_in_urb(&rt->pcm_in_urbs[i], chip);
		if (ret < 0)
			goto error;
	}

	/* Initialize output URBs */
	if (chip->info->out_isoc) {
		/* Unused, but ozzy_pcm_kill_urbs() waits on their anchors */
		for (i = 0; i < OZZY_PCM_N_URBS; i++)
			init_usb_anchor(&rt->pcm_out_urbs[i].submitted);
		ret = ozzy_pcm_init_iso_urbs(chip);
		if (ret < 0)
			goto error;
	} else {
		for (i = 0; i < OZZY_PCM_N_URBS; i++) {
			ret = ozzy_pcm_init_out_urb(&rt->pcm_out_urbs[i], chip);
			if (ret < 0)
				goto error;
		}
	}

	/* Submit all URBs */
	mutex_lock(&rt->stream_mutex);
	for (i = 0; i < OZZY_PCM_N_URBS; i++) {
		usb_anchor_urb(&rt->pcm_in_urbs[i].instance,
			       &rt->pcm_in_urbs[i].submitted);
		ret = usb_submit_urb(&rt->pcm_in_urbs[i].instance, GFP_ATOMIC);
		if (ret < 0) {
			ozzy_pcm_stream_stop(rt);
			ozzy_pcm_kill_urbs(rt);
			goto error_locked;
		}
	}

	if (chip->info->out_isoc) {
		for (i = 0; i < OZZY_PCM_N_FB_URBS && chip->feedback_ep; i++) {
			ret = usb_submit_urb(rt->pcm_fb_urbs[i].instance, GFP_ATOMIC);
			if (ret < 0) {
				ozzy_pcm_stream_stop(rt);
				ozzy_pcm_kill_urbs(rt);
				goto error_locked;
			}
		}
		for (i = 0; i < OZZY_PCM_N_ISO_URBS; i++) {
			ret = usb_submit_urb(rt->pcm_iso_out_urbs[i].instance, GFP_ATOMIC);
			if (ret < 0) {
				ozzy_pcm_stream_stop(rt);
				ozzy_pcm_kill_urbs(rt);
				goto error_locked;
			}
		}
	} else {
		for (i = 0; i < OZZY_PCM_N_URBS; i++) {
			usb_anchor_urb(&rt->pcm_out_urbs[i].instance,
				       &rt->pcm_out_urbs[i].submitted);
			ret = usb_submit_urb(&rt->pcm_out_urbs[i].instance, GFP_ATOMIC);
			if (ret < 0) {
				ozzy_pcm_stream_stop(rt);
				ozzy_pcm_kill_urbs(rt);
				goto error_locked;
			}
		}
	}
	mutex_unlock(&rt->stream_mutex);

	return 0;

error_locked:
	mutex_unlock(&rt->stream_mutex);
error:
	ozzy_pcm_err(&chip->dev->dev, "PCM URB initialization failed\n");
	for (i = 0; i < OZZY_PCM_N_URBS; i++) {
		kfree(rt->pcm_out_urbs[i].buffer);
		kfree(rt->pcm_in_urbs[i].buffer);
	}
	return ret;
}

/* ========================================================================
 * Init / Abort
 * ======================================================================== */

/*
 * ozzy_pcm_drain - Stop resubmitting and let in-flight URBs complete.
 * Killing a URB mid-packet can wedge the Xone:4D's audio engine until the
 * mixer is power-cycled, and the PCM streams always run. So an unbind while
 * the device stays connected (rmmod, or a userspace driver taking it over
 * through usbfs) must let the queued packets finish first. Each URB takes
 * under 2 ms, so 20 ms covers the whole queue.
 */
void ozzy_pcm_drain(struct ozzy_chip *chip)
{
	struct pcm_runtime *rt = chip->pcm;

	if (!rt || rt->panic)
		return;

	rt->stream_state = STREAM_STOPPING;
	msleep(20);
}

/*
 * ozzy_pcm_abort - Emergency stop all PCM activity.
 * Sets panic flag and poisons all URBs to prevent resubmission.
 */
void ozzy_pcm_abort(struct ozzy_chip *chip)
{
	struct pcm_runtime *rt = chip->pcm;

	if (rt) {
		rt->panic = true;
		ozzy_pcm_stream_stop(rt);
		ozzy_pcm_poison_urbs(rt);
	}
}

/*
 * ozzy_pcm_free - Free the isochronous URBs (bulk/interrupt URBs are embedded).
 */
void ozzy_pcm_free(struct ozzy_chip *chip)
{
	struct pcm_runtime *rt = chip->pcm;
	unsigned int i;

	if (!rt)
		return;
	for (i = 0; i < OZZY_PCM_N_ISO_URBS; i++) {
		usb_free_urb(rt->pcm_iso_out_urbs[i].instance);
		kfree(rt->pcm_iso_out_urbs[i].buffer);
		rt->pcm_iso_out_urbs[i].instance = NULL;
		rt->pcm_iso_out_urbs[i].buffer = NULL;
	}
	for (i = 0; i < OZZY_PCM_N_FB_URBS; i++) {
		usb_free_urb(rt->pcm_fb_urbs[i].instance);
		kfree(rt->pcm_fb_urbs[i].buffer);
		rt->pcm_fb_urbs[i].instance = NULL;
		rt->pcm_fb_urbs[i].buffer = NULL;
	}
}

/*
 * ozzy_pcm_init - Create and initialize the PCM subsystem.
 * Allocates the runtime, registers ALSA PCM, and sets up URBs.
 */
int ozzy_pcm_init(struct ozzy_chip *chip)
{
	struct snd_pcm *pcm;
	struct pcm_runtime *rt;
	int ret;

	rt = kzalloc(sizeof(struct pcm_runtime), GFP_KERNEL);
	if (!rt)
		return -ENOMEM;

	rt->chip = chip;
	rt->stream_state = STREAM_DISABLED;

	mutex_init(&rt->stream_mutex);
	spin_lock_init(&rt->playback.lock);
	spin_lock_init(&rt->capture.lock);

	ret = snd_pcm_new(chip->card, chip->dev->product, 0, 1, 1, &pcm);
	if (ret < 0) {
		kfree(rt);
		ozzy_pcm_err(&chip->dev->dev, "Cannot create PCM instance\n");
		return ret;
	}

	pcm->private_data = rt;

	strscpy(pcm->name, chip->dev->product, sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &ozzy_pcm_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &ozzy_pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 0, 0);

	rt->instance = pcm;
	chip->pcm = rt;

	ret = ozzy_pcm_init_urbs(chip);
	if (ret < 0) {
		ozzy_pcm_err(&chip->dev->dev, "PCM URB setup failed\n");
		ozzy_pcm_free(chip);
		chip->pcm = NULL;
		kfree(rt);
		return ret;
	}

	return 0;
}
