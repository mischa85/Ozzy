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
#include "../../common/ozzy_devices.h"

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
	struct ozzy_device_ops ops;     /* the family's ops with this model's packet codec, if it differs */
	const struct ozzy_device_ops *base_ops;
	uint8_t spdif_prev[24];         /* last frame's even half, for the S/PDIF mirror (ESU1808) */
	ploytec_frame_fn encode;        /* frame codecs for this model, picked once */
	ploytec_frame_fn decode;
	struct ploytec_midi midi;       /* MIDI framing at the current rate */
	unsigned int rates[OZZY_MAX_RATES];  /* this device's rates (common/ozzy_devices.def) */
};

static unsigned int ploytec_process_out_packet_spdif12(struct ozzy_chip *chip,
							uint8_t *urb_buf, uint8_t *dma_area,
							unsigned int dma_off,
							unsigned int pcm_buffer_size);
static enum ploytec_out_layout ploytec_out_layout(const struct ozzy_chip *chip);

/* Placeholder until ploytec_init() applies the device's rates (common/ozzy_devices.def) */
static const unsigned int ploytec_rates[] = { 44100, 48000, 88200, 96000 };

/*
 * ploytec_apply_model - Give this chip its own device info with the device's channel
 * counts and sample rates (common/ozzy_devices.def, shared with macOS).
 */
static int ploytec_apply_model(struct ozzy_chip *chip, struct ploytec_private *priv)
{
	const struct ozzy_device *dev =
		ozzy_device_lookup(le16_to_cpu(chip->dev->descriptor.idVendor),
				   le16_to_cpu(chip->dev->descriptor.idProduct));
	unsigned int i, n;

	n = dev ? ozzy_device_rates(dev, priv->rates) : 0;
	if (!n) {
		ploytec_err(&chip->dev->dev, "device not in the device list, or without rates\n");
		return -ENODEV;
	}

	chip->quirks = dev->quirks;
	priv->base_info = chip->info;
	priv->info = *chip->info;
	priv->info.capture_channels = dev->in_channels;
	priv->info.playback_channels = dev->out_channels;
	priv->info.rates = priv->rates;
	priv->info.num_rates = n;
	priv->info.rates_mask = 0;
	for (i = 0; i < n; i++)
		priv->info.rates_mask |= snd_pcm_rate_to_rate_bit(priv->rates[i]);
	priv->info.rate_min = priv->rates[0];
	priv->info.rate_max = priv->rates[n - 1];
	priv->encode = ploytec_frame_encoder(dev->out_channels);
	priv->decode = ploytec_frame_decoder(dev->in_channels);

	chip->info = &priv->info;

	/* ESU1808: its own packet codec (S/PDIF mirror), picked here so the I/O path doesn't branch */
	if ((dev->quirks & OZZY_QUIRK_SPDIF_MIRROR_12) && dev->out_channels == 8 &&
	    ploytec_out_layout(chip) == PLOYTEC_OUT_LAYOUT_BULK) {
		priv->base_ops = chip->ops;
		priv->ops = *chip->ops;
		priv->ops.process_out_packet = ploytec_process_out_packet_spdif12;
		chip->ops = &priv->ops;
	}
	chip->requested_rate = n - 1;

	ploytec_log(&chip->dev->dev, "model: %s, %u in / %u out%s\n", dev->name,
		    dev->in_channels, dev->out_channels,
		    dev->status == OZZY_STATUS_EXPERIMENTAL ? " (experimental)" : "");
	return 0;
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
	if (ploytec_apply_model(chip, priv) < 0)
		return -ENODEV;
	if (!priv->encode || !priv->decode) {
		ploytec_err(&chip->dev->dev, "no frame codec for %u in / %u out\n",
			    chip->info->capture_channels, chip->info->playback_channels);
		return -EINVAL;
	}
	return 0;
}

/*
 * ploytec_adopt_rate - After the handshake: the rate it set (the S/PDIF lock rate while
 * locked) becomes the current rate, and the chip gets the clock status.
 */
static int ploytec_adopt_rate(struct ozzy_chip *chip, const struct ploytec_handshake_info *hs)
{
	unsigned int i;

	chip->clock = hs->clock;
	for (i = 0; i < chip->info->num_rates; i++) {
		if (chip->info->rates[i] == hs->rate) {
			chip->requested_rate = i;
			chip->current_rate = i;
			return 0;
		}
	}
	ploytec_err(&chip->dev->dev, "device runs at %u Hz, which it doesn't list\n", hs->rate);
	chip->clock.locked = false;
	return -EINVAL;
}

/*
 * ploytec_init - Perform the Ploytec vendor handshake after USB probe or reset.
 * On the first call, allocates private data and applies the model. Then runs the
 * shared handshake at the requested rate (or the S/PDIF lock rate) with the
 * chip's settings. Private data is kept on failure because
 * chip->info points into it; ploytec_free() releases it.
 */
static int ploytec_init(struct ozzy_chip *chip)
{
	struct ploytec_handshake_info hs;
	struct ploytec_private *priv;
	int ret;

	ret = ploytec_private_init(chip);
	if (ret < 0)
		return ret;
	priv = chip->private_data;

	ret = ploytec_handshake(&ploytec_host_ops, chip, chip->info->rates[chip->requested_rate],
				chip->quirks, &chip->settings, &hs);
	if (ret < 0)
		return ret;
	ret = ploytec_adopt_rate(chip, &hs);
	if (ret < 0)
		return ret;

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
	if (priv->base_ops)
		chip->ops = priv->base_ops;
	kfree(priv);
	chip->private_data = NULL;
}

/*
 * ploytec_reset - Perform a USB device reset for sample rate and settings changes.
 * The core's pre_reset/post_reset callbacks handle URB teardown/reinit.
 * usb_reset_device() wants the device lock, which an interface driver takes
 * with usb_lock_device_for_reset().
 */
static int ploytec_reset(struct ozzy_chip *chip)
{
	int ret;

	ploytec_notice(&chip->dev->dev, "resetting device\n");
	ret = usb_lock_device_for_reset(chip->dev, chip->intf);
	if (ret < 0)
		return ret;
	ret = usb_reset_device(chip->dev);
	usb_unlock_device(chip->dev);
	return ret;
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
 * ploytec_process_out_packet_spdif12 - ploytec_process_out_packet for the ESU1808 (bulk
 * layout): 8 channels plus the S/PDIF mirror of pair 1/2 in lanes 9/10, which takes
 * channel 2 from the frame before (the previous packet's last frame for the first one).
 */
static unsigned int ploytec_process_out_packet_spdif12(struct ozzy_chip *chip,
							uint8_t *urb_buf, uint8_t *dma_area,
							unsigned int dma_off,
							unsigned int pcm_buffer_size)
{
	struct ploytec_private *priv = chip->private_data;
	unsigned int frame_bytes = 8 * ALSA_BYTES_PER_SAMPLE;
	const uint8_t *prev = priv->spdif_prev;
	unsigned int f, src_off;
	uint8_t *dest;

	for (f = 0; f < PLOYTEC_FRAMES_PER_PKT; f++) {
		src_off = dma_off + f * frame_bytes;
		if (src_off >= pcm_buffer_size)
			src_off -= pcm_buffer_size;

		dest = urb_buf + ploytec_out_frame_offset(PLOYTEC_OUT_LAYOUT_BULK, f);
		ploytec_encode_frame_8ch(dest, dma_area + src_off);
		ploytec_spdif_mirror_12(dest, prev);
		prev = dest + PLOYTEC_OUT_FRAME_SIZE / 2;
	}
	memcpy(priv->spdif_prev, prev, sizeof(priv->spdif_prev));

	return PLOYTEC_FRAMES_PER_PKT * frame_bytes;
}

/*
 * ploytec_process_in_packet - Decode a Ploytec input packet into ALSA audio.
 * Zero-copy: reads from URB buffer, writes directly to ALSA DMA area.
 * Handles DMA ring buffer wraparound. Decodes only the whole frames among the
 * @len bytes received (frames are contiguous, PLOYTEC_IN_FRAME_SIZE each).
 */
static unsigned int ploytec_process_in_packet(struct ozzy_chip *chip,
					      uint8_t *urb_buf, unsigned int len,
					      uint8_t *dma_area,
					      unsigned int dma_off,
					      unsigned int pcm_buffer_size)
{
	struct ploytec_private *priv = chip->private_data;
	unsigned int frame_bytes = chip->info->capture_channels * ALSA_BYTES_PER_SAMPLE;
	unsigned int frames = min_t(unsigned int, len / PLOYTEC_IN_FRAME_SIZE, PLOYTEC_FRAMES_PER_PKT);
	unsigned int f;
	unsigned int dst_off;

	for (f = 0; f < frames; f++) {
		dst_off = dma_off + f * frame_bytes;
		if (dst_off >= pcm_buffer_size)
			dst_off -= pcm_buffer_size;

		priv->decode(dma_area + dst_off, urb_buf + f * PLOYTEC_IN_FRAME_SIZE);
	}

	return frames * frame_bytes;
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
	struct ploytec_handshake_info hs;
	struct ploytec_private *priv;
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

	ret = ploytec_handshake_eps(&ploytec_isoc_host_ops, chip,
				    chip->info->rates[chip->requested_rate],
				    &ploytec_isoc_rate_endpoints, chip->quirks, &chip->settings, &hs);
	if (ret < 0)
		return ret;
	ret = ploytec_adopt_rate(chip, &hs);
	if (ret < 0)
		return ret;

	/* MIDI framing on its own endpoints, as the device's kInACR picks it (ploytec_midi.h) */
	ploytec_midi_init_xfer(&priv->midi, hs.rate, (chip->quirks & OZZY_QUIRK_MIDI_NS7) ?
			       PLOYTEC_MIDI_XFER_NS7 : PLOYTEC_MIDI_XFER_M4_C1);
	priv->info.midi_in_size = priv->midi.xfer_size;
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
	return priv->midi.xfer_size;
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
	.out_ep                = { .address = PLOYTEC_EP_PCM_OUT },
	.in_ep                 = { .address = PLOYTEC_EP_PCM_IN | USB_DIR_IN },
	.alsa_format           = SNDRV_PCM_FMTBIT_S24_3LE,
	.bytes_per_sample      = 3,
	.midi_in_ep            = { .address = PLOYTEC_EP_MIDI_IN | USB_DIR_IN },
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
 * ploytec_apply_model(). Endpoints and feedback as both platforms describe them
 * (ploytec_defs.h).
 */
const struct ozzy_device_info ploytec_isoc_info = {
	.name                  = "Ploytec isochronous out",
	.playback_channels     = 4,
	.capture_channels      = 4,
	.in_packet_size        = PLOYTEC_IN_PKT_SIZE,
	.frames_per_in_packet  = PLOYTEC_FRAMES_PER_PKT,
	.out_ep                = PLOYTEC_ISOC_EP_PCM_OUT,
	.in_ep                 = PLOYTEC_ISOC_EP_PCM_IN,
	.alsa_format           = SNDRV_PCM_FMTBIT_S24_3LE,
	.bytes_per_sample      = 3,
	.out_isoc              = true,
	.feedback_ep           = PLOYTEC_ISOC_EP_FEEDBACK,
	.feedback              = PLOYTEC_ISOC_FEEDBACK,
	.midi_in_ep            = PLOYTEC_ISOC_EP_MIDI_IN,
	.midi_out_ep           = PLOYTEC_ISOC_EP_MIDI_OUT,
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
