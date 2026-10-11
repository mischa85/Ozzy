// SPDX-License-Identifier: MIT
/*
 * Ploytec Device Family - Allen & Heath Xone Series
 *
 * Device-specific implementation for the Ploytec USB audio chipset used
 * in Allen & Heath Xone:DB4, DB2, DX, 4D, and Wizard 4 mixers.
 *
 * Handles: vendor request handshake, sample rate control, Ploytec
 * bit-interleaved PCM codec, packet framing with padding gaps, and
 * MIDI byte embedding in PCM output packets.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/usb.h>

#include "../ozzy.h"
#include "../ozzy_log.h"
#include "../ozzy_pcm.h"
#include "../ozzy_midi.h"
#include "ploytec.h"
#include "../../common/devices/ploytec/ploytec_defs.h"
#include "../../common/devices/ploytec/ploytec_protocol.h"
#include "../../common/devices/ploytec/ploytec_codec.h"
#include "../../common/devices/ploytec/ploytec_handshake.h"
#include "../../common/devices/ploytec/ploytec_models.h"

/* ALSA side: S24_3LE, 3 bytes per sample */
#define ALSA_BYTES_PER_SAMPLE 3

/*
 * struct ploytec_private - Ploytec device-specific runtime data.
 *
 * All buffers used with usb_control_msg must be heap-allocated (DMA-safe).
 * Stack buffers cause "transfer buffer is on stack" kernel warnings/crashes.
 */
struct ploytec_private {
	unsigned char xfer_buf[16];     /* DMA-safe bounce buffer for control transfers */
	struct ozzy_device_info info;   /* ploytec_info with this model's channels and rates */
	const struct ozzy_device_info *base_info;
};

/* Placeholder until ploytec_init() applies the model's rates (ploytec_models.h) */
static const unsigned int ploytec_rates[] = { 44100, 48000, 88200, 96000 };

/*
 * ploytec_apply_model - Give this chip its own device info with the model's channel
 * counts and sample rates (from ploytec_models.h, shared with macOS).
 */
static void ploytec_apply_model(struct ozzy_chip *chip, struct ploytec_private *priv)
{
	const struct ploytec_model *model =
		ploytec_model_for_pid(le16_to_cpu(chip->dev->descriptor.idProduct));
	unsigned int i;

	priv->base_info = chip->info;
	priv->info = *chip->info;
	priv->info.capture_channels = model->in_channels;
	priv->info.playback_channels = model->out_channels;
	priv->info.rates = model->rates;
	priv->info.num_rates = model->num_rates;
	priv->info.rates_mask = 0;
	for (i = 0; i < model->num_rates; i++)
		priv->info.rates_mask |= snd_pcm_rate_to_rate_bit(model->rates[i]);
	priv->info.rate_min = model->rates[0];
	priv->info.rate_max = model->rates[model->num_rates - 1];

	chip->info = &priv->info;
	chip->requested_rate = model->num_rates - 1;

	ploytec_log(&chip->dev->dev, "model: %s, %u in / %u out\n",
		    model->name, model->in_channels, model->out_channels);
}

/*
 * ploytec_out_layout - Output packet layout for this device (follows the PCM OUT
 * endpoint's wMaxPacketSize, see ploytec_protocol.h).
 */
static enum ploytec_out_layout ploytec_out_layout(const struct ozzy_chip *chip)
{
	return ploytec_out_layout_for_maxpacket(chip->out_maxpacket);
}

/* ========================================================================
 * Host Ops for the Shared Handshake (common/devices/ploytec/ploytec_handshake.c)
 * ======================================================================== */

/*
 * ploytec_host_control - EP0 control transfer through the DMA-safe bounce buffer.
 */
static int ploytec_host_control(void *ctx, uint8_t request_type, uint8_t request,
				uint16_t value, uint16_t index, void *data, uint16_t length)
{
	struct ozzy_chip *chip = ctx;
	struct ploytec_private *priv = chip->private_data;
	bool in = request_type & USB_DIR_IN;
	int ret;

	if (length > sizeof(priv->xfer_buf))
		return -EINVAL;
	if (!in && length)
		memcpy(priv->xfer_buf, data, length);

	ret = usb_control_msg(chip->dev,
			      in ? usb_rcvctrlpipe(chip->dev, 0) : usb_sndctrlpipe(chip->dev, 0),
			      request, request_type, value, index,
			      length ? priv->xfer_buf : NULL, length, 2000);

	if (in && ret > 0)
		memcpy(data, priv->xfer_buf, min_t(int, ret, length));
	return ret;
}

static int ploytec_host_clear_halt(void *ctx, uint8_t endpoint)
{
	struct ozzy_chip *chip = ctx;
	unsigned int ep = endpoint & USB_ENDPOINT_NUMBER_MASK;

	return usb_clear_halt(chip->dev, (endpoint & USB_DIR_IN) ?
			      usb_rcvintpipe(chip->dev, ep) : usb_sndintpipe(chip->dev, ep));
}

static void ploytec_host_sleep_ms(void *ctx, unsigned int ms)
{
	msleep(ms);
}

static void ploytec_host_vlog(void *ctx, bool error, const char *fmt, va_list args)
{
	struct ozzy_chip *chip = ctx;
	struct va_format vaf;
	va_list copy;

	/* va_list parameters decay to a pointer on some ABIs; %pV needs a real va_list */
	va_copy(copy, args);
	vaf.fmt = fmt;
	vaf.va = &copy;
	if (error)
		ploytec_err(&chip->dev->dev, "%pV\n", &vaf);
	else
		ploytec_log(&chip->dev->dev, "%pV\n", &vaf);
	va_end(copy);
}

static const struct ploytec_host_ops ploytec_host_ops = {
	.control    = ploytec_host_control,
	.clear_halt = ploytec_host_clear_halt,
	.sleep_ms   = ploytec_host_sleep_ms,
	.vlog       = ploytec_host_vlog,
};

/* ========================================================================
 * Device Ops Implementation
 * ======================================================================== */

/*
 * ploytec_init - Perform the Ploytec vendor handshake after USB probe or reset.
 * On the first call, allocates private data and applies the model. Then runs the
 * shared handshake at the requested rate. Private data is kept on failure because
 * chip->info points into it; ploytec_free() releases it.
 */
static int ploytec_init(struct ozzy_chip *chip)
{
	struct ploytec_private *priv = chip->private_data;
	int ret;

	if (!priv) {
		priv = kzalloc(sizeof(*priv), GFP_KERNEL);
		if (!priv)
			return -ENOMEM;
		chip->private_data = priv;
		ploytec_apply_model(chip, priv);
	}

	ret = ploytec_handshake(&ploytec_host_ops, chip,
				chip->info->rates[chip->requested_rate], NULL);
	if (ret < 0)
		return ret;

	chip->current_rate = chip->requested_rate;
	return 0;
}

/*
 * ploytec_free - Release Ploytec private data.
 */
static void ploytec_free(struct ozzy_chip *chip)
{
	struct ploytec_private *priv = chip->private_data;

	if (!priv)
		return;
	chip->info = priv->base_info;
	kfree(priv);
	chip->private_data = NULL;
}

/*
 * ploytec_reset - Perform a USB device reset for sample rate changes.
 * The core's pre_reset/post_reset callbacks handle URB teardown/reinit.
 */
static int ploytec_reset(struct ozzy_chip *chip)
{
	ploytec_notice(&chip->dev->dev, "resetting device for sample rate change\n");
	return usb_reset_device(chip->dev);
}

/* ========================================================================
 * PCM Packet Processing (zero-copy, directly between URB and DMA buffers)
 * ======================================================================== */

/*
 * ploytec_process_out_packet - Encode ALSA audio into a Ploytec output packet.
 * Frame offsets come from the shared layout helpers (ploytec_protocol.h).
 * Zero-copy: reads from the ALSA DMA area, writes directly into the URB buffer.
 * Handles DMA ring buffer wraparound.
 */
static unsigned int ploytec_process_out_packet(struct ozzy_chip *chip,
					       uint8_t *urb_buf, uint8_t *dma_area,
					       unsigned int dma_off,
					       unsigned int pcm_buffer_size)
{
	enum ploytec_out_layout layout = ploytec_out_layout(chip);
	unsigned int channels = chip->info->playback_channels;
	unsigned int frame_bytes = channels * ALSA_BYTES_PER_SAMPLE;
	unsigned int f, src_off;

	for (f = 0; f < PLOYTEC_FRAMES_PER_PKT; f++) {
		src_off = dma_off + f * frame_bytes;
		if (src_off >= pcm_buffer_size)
			src_off -= pcm_buffer_size;

		ploytec_encode_frame(urb_buf + ploytec_out_frame_offset(layout, f),
				     dma_area + src_off, channels);
	}

	return PLOYTEC_FRAMES_PER_PKT * frame_bytes;
}

/*
 * ploytec_process_in_packet - Decode a Ploytec input packet into ALSA audio.
 * Zero-copy: reads from URB buffer, writes directly to ALSA DMA area.
 * Handles DMA ring buffer wraparound.
 */
static unsigned int ploytec_process_in_packet(struct ozzy_chip *chip,
					      uint8_t *urb_buf, uint8_t *dma_area,
					      unsigned int dma_off,
					      unsigned int pcm_buffer_size)
{
	unsigned int channels = chip->info->capture_channels;
	unsigned int frame_bytes = channels * ALSA_BYTES_PER_SAMPLE;
	unsigned int f;
	unsigned int dst_off;

	for (f = 0; f < PLOYTEC_FRAMES_PER_PKT; f++) {
		dst_off = dma_off + f * frame_bytes;
		if (dst_off >= pcm_buffer_size)
			dst_off -= pcm_buffer_size;

		ploytec_decode_frame(dma_area + dst_off,
				     urb_buf + f * PLOYTEC_IN_FRAME_SIZE, channels);
	}

	return PLOYTEC_FRAMES_PER_PKT * frame_bytes;
}

/* ========================================================================
 * URB Initialization and MIDI Embedding
 * ======================================================================== */

/*
 * ploytec_init_out_urb - Fill an output URB with the Ploytec silence pattern.
 */
static void ploytec_init_out_urb(struct ozzy_chip *chip, uint8_t *buffer)
{
	ploytec_out_init_packet(ploytec_out_layout(chip), buffer);
}

/*
 * ploytec_fill_midi_out - Embed the next pending MIDI byte into a PCM output packet.
 * See ploytec_out_set_midi() for why it is one byte per packet.
 */
static void ploytec_fill_midi_out(struct ozzy_chip *chip, uint8_t *urb_buf)
{
	struct midi_runtime *rt = chip->midi;
	u8 byte;

	if (!rt)
		return;

	ozzy_midi_consume(rt, &byte, 1, PLOYTEC_MIDI_IDLE_BYTE);
	ploytec_out_set_midi(ploytec_out_layout(chip), urb_buf, byte);
}

/*
 * ploytec_get_out_packet_size - Return output packet size for this device's layout.
 */
static unsigned int ploytec_get_out_packet_size(struct ozzy_chip *chip, bool is_bulk)
{
	return ploytec_out_packet_size(ploytec_out_layout(chip));
}

/* ========================================================================
 * Exported Device Descriptor and Operations
 * ======================================================================== */

const struct ozzy_device_info ploytec_info = {
	.name                  = "Ploytec Xone",
	.playback_channels     = PLOYTEC_MAX_OUT_CHANNELS,   /* per model, see ploytec_apply_model() */
	.capture_channels      = PLOYTEC_MAX_OUT_CHANNELS,
	.out_packet_size       = PLOYTEC_BULK_OUT_PKT_SIZE,
	.in_packet_size        = PLOYTEC_IN_PKT_SIZE,
	.frames_per_out_packet = PLOYTEC_FRAMES_PER_PKT,
	.frames_per_in_packet  = PLOYTEC_FRAMES_PER_PKT,
	.out_ep                = PLOYTEC_EP_PCM_OUT,
	.in_ep                 = PLOYTEC_EP_PCM_IN,
	.alsa_format           = SNDRV_PCM_FMTBIT_S24_3LE,
	.bytes_per_sample      = 3,
	.midi_in_ep            = PLOYTEC_EP_MIDI_IN,
	.midi_out_embedded     = true,
	.num_interfaces        = PLOYTEC_NUM_INTERFACES,
	.alt_setting           = PLOYTEC_ALT_SETTING,
	.rates                 = ploytec_rates,
	.num_rates             = ARRAY_SIZE(ploytec_rates),
	.rates_mask            = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000,
	.rate_min              = 44100,
	.rate_max              = 96000,
};

const struct ozzy_device_ops ploytec_ops = {
	.init                = ploytec_init,
	.free                = ploytec_free,
	.reset               = ploytec_reset,
	.process_out_packet  = ploytec_process_out_packet,
	.process_in_packet   = ploytec_process_in_packet,
	.init_out_urb        = ploytec_init_out_urb,
	.fill_midi_out       = ploytec_fill_midi_out,
	.get_out_packet_size = ploytec_get_out_packet_size,
};
