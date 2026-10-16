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
 * Also the bulk in / isochronous out variant (Akai EIE Pro, ploytec_isoc_ops):
 * same handshake, PCM in and MIDI in, but plain S24_3LE PCM out on an
 * isochronous endpoint paced by a feedback endpoint (run by the core, see
 * common/isoc_feedback.h), and MIDI out on its own endpoint (ploytec_midi.h).
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
#include "../../common/devices/ploytec/ploytec_midi.h"
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
	ploytec_frame_fn encode;        /* frame codecs for this model, picked once */
	ploytec_frame_fn decode;
	struct ploytec_midi midi;       /* MIDI framing at the current rate */
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
		ploytec_model_for_ids(le16_to_cpu(chip->dev->descriptor.idVendor),
				      le16_to_cpu(chip->dev->descriptor.idProduct));
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
	priv->encode = ploytec_frame_encoder(model->out_channels);
	priv->decode = ploytec_frame_decoder(model->in_channels);

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
 * ploytec_pcm_in_int_1024 - Whether the PCM IN endpoint is interrupt with
 * wMaxPacketSize 1024 (selects the MIDI IN frame above 48 kHz, ploytec_midi.h).
 */
static bool ploytec_pcm_in_int_1024(struct ozzy_chip *chip)
{
	struct usb_host_endpoint *ep = chip->dev->ep_in[chip->in_ep];

	return ep && usb_endpoint_xfer_int(&ep->desc) && usb_endpoint_maxp(&ep->desc) == 1024;
}

/*
 * ploytec_private_init - On the first init: allocate private data and apply the
 * model (channels, rates, frame codecs). Kept on failure because chip->info
 * points into it; ploytec_free() releases it.
 */
static int ploytec_private_init(struct ozzy_chip *chip)
{
	struct ploytec_private *priv;

	if (chip->private_data)
		return 0;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	chip->private_data = priv;
	ploytec_apply_model(chip, priv);
	if (!priv->encode || !priv->decode) {
		ploytec_err(&chip->dev->dev, "no frame codec for %u in / %u out\n",
			    chip->info->capture_channels, chip->info->playback_channels);
		return -EINVAL;
	}
	return 0;
}

/*
 * ploytec_init - Perform the Ploytec vendor handshake after USB probe or reset.
 * On the first call, allocates private data and applies the model. Then runs the
 * shared handshake at the requested rate. Private data is kept on failure because
 * chip->info points into it; ploytec_free() releases it.
 */
static int ploytec_init(struct ozzy_chip *chip)
{
	struct ploytec_private *priv;
	int ret;

	ret = ploytec_private_init(chip);
	if (ret < 0)
		return ret;
	priv = chip->private_data;

	ret = ploytec_handshake(&ploytec_host_ops, chip,
				chip->info->rates[chip->requested_rate], NULL);
	if (ret < 0)
		return ret;

	chip->current_rate = chip->requested_rate;
	ploytec_midi_init(&priv->midi, chip->info->rates[chip->current_rate],
			  ploytec_pcm_in_int_1024(chip));
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
	struct ploytec_private *priv = chip->private_data;
	enum ploytec_out_layout layout = ploytec_out_layout(chip);
	unsigned int frame_bytes = chip->info->playback_channels * ALSA_BYTES_PER_SAMPLE;
	unsigned int f, src_off;

	for (f = 0; f < PLOYTEC_FRAMES_PER_PKT; f++) {
		src_off = dma_off + f * frame_bytes;
		if (src_off >= pcm_buffer_size)
			src_off -= pcm_buffer_size;

		priv->encode(urb_buf + ploytec_out_frame_offset(layout, f), dma_area + src_off);
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
	struct ploytec_private *priv = chip->private_data;
	unsigned int frame_bytes = chip->info->capture_channels * ALSA_BYTES_PER_SAMPLE;
	unsigned int f;
	unsigned int dst_off;

	for (f = 0; f < PLOYTEC_FRAMES_PER_PKT; f++) {
		dst_off = dma_off + f * frame_bytes;
		if (dst_off >= pcm_buffer_size)
			dst_off -= pcm_buffer_size;

		priv->decode(dma_area + dst_off, urb_buf + f * PLOYTEC_IN_FRAME_SIZE);
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
 * ploytec_fill_midi_out - Embed pending MIDI bytes into a PCM output packet,
 * in the sub-packets the MIDI out pace opens (ploytec_midi.h).
 */
static void ploytec_fill_midi_out(struct ozzy_chip *chip, uint8_t *urb_buf)
{
	struct ploytec_private *priv = chip->private_data;
	u8 bytes[PLOYTEC_OUT_SUBPKTS] = { 0 };
	u32 mask, count;

	if (!chip->midi)
		return;

	count = ploytec_midi_out_slots(&priv->midi, &mask);
	if (count)
		ozzy_midi_consume(chip->midi, bytes, count, PLOYTEC_MIDI_IDLE_BYTE);
	ploytec_midi_out_place(ploytec_out_layout(chip), urb_buf, mask, bytes);
}

/*
 * ploytec_decode_midi_in - The MIDI bytes of a MIDI IN transfer (ploytec_midi.h).
 */
static unsigned int ploytec_decode_midi_in(struct ozzy_chip *chip, const uint8_t *buf,
					   unsigned int len, uint8_t *out)
{
	struct ploytec_private *priv = chip->private_data;

	return ploytec_midi_in(&priv->midi, buf, len, out);
}

/*
 * ploytec_get_out_packet_size - Return output packet size for this device's layout.
 */
static unsigned int ploytec_get_out_packet_size(struct ozzy_chip *chip, bool is_bulk)
{
	return ploytec_out_packet_size(ploytec_out_layout(chip));
}

/* ========================================================================
 * Bulk In / Isochronous Out (Akai EIE Pro)
 * ======================================================================== */

/* Host ops without clear_halt: the official driver doesn't clear halts on these devices */
static const struct ploytec_host_ops ploytec_isoc_host_ops = {
	.control  = ploytec_host_control,
	.sleep_ms = ploytec_host_sleep_ms,
	.vlog     = ploytec_host_vlog,
};

/*
 * ploytec_isoc_init - Bulk in / isoc out device after probe or reset: the
 * handshake with the rate on EP 0x86 / 0x02, and MIDI framing at the rate.
 * The core runs the isochronous output.
 */
static int ploytec_isoc_init(struct ozzy_chip *chip)
{
	struct ploytec_private *priv;
	uint32_t rate;
	int ret;

	/* The 4-channel format table is the High Speed one (the Full Speed one is 2 channels) */
	if (chip->dev->speed != USB_SPEED_HIGH) {
		ploytec_err(&chip->dev->dev, "needs a USB 2.0 High Speed port\n");
		return -ENODEV;
	}

	ret = ploytec_private_init(chip);
	if (ret < 0)
		return ret;
	priv = chip->private_data;

	rate = chip->info->rates[chip->requested_rate];
	ret = ploytec_handshake_eps(&ploytec_isoc_host_ops, chip, rate,
				    &ploytec_isoc_rate_endpoints, NULL);
	if (ret < 0)
		return ret;

	chip->current_rate = chip->requested_rate;
	/* kInACR 3: the 4-slot MIDI IN frame (inACR_M4_C1) at every rate */
	ploytec_midi_init_slots(&priv->midi, rate, PLOYTEC_MIDI_IN_MAX_SLOTS);
	return 0;
}

/*
 * ploytec_fill_midi_out_xfer - The next MIDI OUT transfer on its own endpoint
 * (ploytec_midi.h).
 */
static unsigned int ploytec_fill_midi_out_xfer(struct ozzy_chip *chip, uint8_t *buf)
{
	struct ploytec_private *priv = chip->private_data;
	unsigned int n = ploytec_midi_xfer_begin(&priv->midi, buf);

	ozzy_midi_consume(chip->midi, buf, n, PLOYTEC_MIDI_IDLE_BYTE);
	return PLOYTEC_MIDI_XFER_SIZE;
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
	.out_ep                = { .num = PLOYTEC_EP_PCM_OUT },
	.in_ep                 = { .num = PLOYTEC_EP_PCM_IN },
	.alsa_format           = SNDRV_PCM_FMTBIT_S24_3LE,
	.bytes_per_sample      = 3,
	.midi_in_ep            = { .num = PLOYTEC_EP_MIDI_IN },
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
	.decode_midi_in      = ploytec_decode_midi_in,
	.get_out_packet_size = ploytec_get_out_packet_size,
};

/*
 * Bulk in / isochronous out (Akai EIE Pro); channels and rates per model, see
 * ploytec_apply_model(). Endpoints are found by type, like the official driver
 * does (on the EIE Pro: PCM out 0x02, MIDI out 0x04, MIDI in 0x83 on interface 0;
 * feedback 0x81, PCM in 0x86 on interface 1).
 */
const struct ozzy_device_info ploytec_isoc_info = {
	.name                  = "Ploytec isochronous out",
	.playback_channels     = 4,
	.capture_channels      = 4,
	.in_packet_size        = PLOYTEC_IN_PKT_SIZE,
	.frames_per_in_packet  = PLOYTEC_FRAMES_PER_PKT,
	.out_ep                = { .intf = 0, .types = OZZY_EP_ISOC },
	.in_ep                 = { .intf = 1, .types = OZZY_EP_BULK | OZZY_EP_INT },
	.alsa_format           = SNDRV_PCM_FMTBIT_S24_3LE,
	.bytes_per_sample      = 3,
	.out_isoc              = true,
	.feedback_ep           = { .intf = 1, .types = OZZY_EP_ISOC },
	.feedback              = { ISOC_FB_COUNT, PLOYTEC_ISOC_FB_PACKET_SIZE, PLOYTEC_ISOC_FB_MAX_ERRORS },
	.midi_in_ep            = { .intf = 0, .types = OZZY_EP_BULK | OZZY_EP_INT },
	.midi_out_ep           = { .intf = 0, .types = OZZY_EP_BULK | OZZY_EP_INT },
	.midi_out_interval_us  = PLOYTEC_MIDI_XFER_INTERVAL_US,
	.num_interfaces        = PLOYTEC_NUM_INTERFACES,
	.alt_setting           = PLOYTEC_ALT_SETTING,
	.rates                 = ploytec_rates,
	.num_rates             = ARRAY_SIZE(ploytec_rates),
	.rates_mask            = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000,
	.rate_min              = 44100,
	.rate_max              = 96000,
};

const struct ozzy_device_ops ploytec_isoc_ops = {
	.init                = ploytec_isoc_init,
	.free                = ploytec_free,
	.reset               = ploytec_reset,
	.process_in_packet   = ploytec_process_in_packet,
	.decode_midi_in      = ploytec_decode_midi_in,
	.fill_midi_out_xfer  = ploytec_fill_midi_out_xfer,
};
