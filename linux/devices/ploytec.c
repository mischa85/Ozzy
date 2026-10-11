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

/* ALSA frame size: 8 channels x 3 bytes (S24_3LE) */
#define ALSA_FRAME_SIZE   (PLOYTEC_CHANNELS * 3)

/* Number of ALSA bytes per output packet */
#define ALSA_OUT_PKT_SIZE (PLOYTEC_FRAMES_PER_PKT * ALSA_FRAME_SIZE)

/* Number of ALSA bytes per input packet */
#define ALSA_IN_PKT_SIZE  (PLOYTEC_FRAMES_PER_PKT * ALSA_FRAME_SIZE)

/*
 * struct ploytec_private - Ploytec device-specific runtime data.
 *
 * All buffers used with usb_control_msg must be heap-allocated (DMA-safe).
 * Stack buffers cause "transfer buffer is on stack" kernel warnings/crashes.
 */
struct ploytec_private {
	unsigned char firmware_ver[15]; /* raw firmware version response */
	unsigned char status[1];        /* hardware status byte */
	unsigned char xfer_buf[16];     /* reusable DMA-safe buffer for vendor requests */
};

/* Supported sample rates */
static const unsigned int ploytec_rates[] = { 44100, 48000, 88200, 96000 };

/*
 * Xone:DB2 is 48 kHz only. This matches the official driver's per-device format
 * table (kConfigurationData in the XONE_DB2.kext plist). The device boots at
 * 44100 Hz but accepts other rates at the USB level without its DSP following.
 */
static const unsigned int ploytec_db2_rates[] = { 48000 };

/* Forward declarations */
static int ploytec_set_rate(struct ozzy_chip *chip, unsigned int rate_index);

/*
 * ploytec_out_layout - Output packet layout for this device (follows the PCM OUT
 * endpoint's wMaxPacketSize, see ploytec_protocol.h).
 */
static enum ploytec_out_layout ploytec_out_layout(const struct ozzy_chip *chip)
{
	return ploytec_out_layout_for_maxpacket(chip->out_maxpacket);
}

/*
 * ploytec_get_firmware - Read the Ploytec firmware version.
 * Stores the 15-byte response in priv->firmware_ver and logs the version.
 */
static int ploytec_get_firmware(struct ozzy_chip *chip)
{
	struct ploytec_private *priv = chip->private_data;
	int ret;

	ret = usb_control_msg(chip->dev, usb_rcvctrlpipe(chip->dev, 0),
			      PLOYTEC_CMD_FIRMWARE, 0xC0, 0x0000, 0,
			      priv->firmware_ver, 15, 2000);
	if (ret < 0)
		return ret;

	{
		struct ploytec_firmware_version fw =
			ploytec_parse_firmware(priv->firmware_ver);
		ploytec_log(&chip->dev->dev,
			    "firmware: v1.%d.%d (chip ID: 0x%02X, raw: %*ph)\n",
			    fw.major, fw.minor, fw.chip_id,
			    15, priv->firmware_ver);
	}

	return 0;
}

/*
 * ploytec_get_status - Read and decode the AJ Input Selector register.
 * See ploytec_aj_input_status for confirmed vs partially understood bits.
 */
static int ploytec_get_status(struct ozzy_chip *chip)
{
	struct ploytec_private *priv = chip->private_data;
	struct ploytec_aj_input_status st;
	int ret;

	ret = usb_control_msg(chip->dev, usb_rcvctrlpipe(chip->dev, 0),
			      PLOYTEC_CMD_STATUS, 0xC0, 0x0000,
			      PLOYTEC_REG_AJ_INPUT_SEL,
			      priv->status, 1, 2000);
	if (ret < 0)
		return ret;

	ploytec_decode_aj_input(priv->status[0], &st);
	ploytec_log(&chip->dev->dev,
		    "status: 0x%02X [InputSel:%d] [DigLock:%d] "
		    "[mode: %d%d%d%d%d%d]\n",
		    st.raw, st.input_select, st.digital_lock,
		    st.mode7, st.mode6, st.mode5,
		    st.mode4, st.mode3, st.mode0);

	return 0;
}

/*
 * ploytec_get_rate - Read the current hardware sample rate and update chip->current_rate.
 * Returns 0 on success, 1 if the rate is not in the device's rate table
 * (e.g. the DB2 boots at 44100 Hz), negative errno on failure.
 */
static int ploytec_get_rate(struct ozzy_chip *chip)
{
	struct ploytec_private *priv = chip->private_data;
	uint32_t rate;
	unsigned int i;
	int ret;

	ret = usb_control_msg(chip->dev, usb_rcvctrlpipe(chip->dev, 0),
			      PLOYTEC_CMD_GET_RATE_REQ, PLOYTEC_CMD_GET_RATE_TYPE,
			      0x0100, 0, priv->xfer_buf, 3, 2000);
	if (ret < 0)
		return ret;

	rate = ploytec_decode_rate(priv->xfer_buf);

	ploytec_log(&chip->dev->dev, "hardware sample rate: %u Hz (raw: %02X %02X %02X)\n",
		    rate, priv->xfer_buf[0], priv->xfer_buf[1], priv->xfer_buf[2]);

	for (i = 0; i < chip->info->num_rates; i++) {
		if (chip->info->rates[i] == rate) {
			chip->current_rate = i;
			return 0;
		}
	}

	ploytec_log(&chip->dev->dev, "hardware rate %u Hz not in rate table\n", rate);
	return 1;
}

/*
 * ploytec_confirm_status - Read-modify-write the AJ Input Selector register.
 *
 * Reads the status byte from vendor request 'I' (wIndex=0), then writes
 * it back with bit 5 set using sign-extension to compute the 16-bit wValue.
 * This completes the handshake and arms the device for streaming.
 */
static int ploytec_confirm_status(struct ozzy_chip *chip)
{
	struct ploytec_private *priv = chip->private_data;
	uint16_t wvalue;
	int ret;

	ret = usb_control_msg(chip->dev, usb_rcvctrlpipe(chip->dev, 0),
			      PLOYTEC_CMD_STATUS, 0xC0, 0x0000,
			      PLOYTEC_REG_AJ_INPUT_SEL,
			      priv->status, 1, 2000);
	if (ret < 0)
		return ret;

	wvalue = ploytec_confirm_wvalue(priv->status[0]);

	ret = usb_control_msg(chip->dev, usb_sndctrlpipe(chip->dev, 0),
			      PLOYTEC_CMD_STATUS, 0x40,
			      wvalue, PLOYTEC_REG_AJ_INPUT_SEL,
			      NULL, 0, 2000);
	if (ret >= 0)
		ploytec_log(&chip->dev->dev,
			    "status confirmed (read=0x%02X, wrote wValue=0x%04X)\n",
			    priv->status[0], wvalue);

	return ret;
}

/*
 * ploytec_clear_pcm_halt - Reset both PCM endpoints before streaming starts.
 * The A&H Windows driver does this after the status confirm (abort + clear
 * ENDPOINT_HALT on EP 0x86, then EP 0x05), seen in captures of the Xone:DB2 and
 * Xone:4D. Without it the 4D never starts streaming on Linux (#75).
 */
static int ploytec_clear_pcm_halt(struct ozzy_chip *chip)
{
	int ret;

	msleep(500);
	ret = usb_clear_halt(chip->dev, usb_rcvintpipe(chip->dev, chip->info->in_ep));
	if (ret < 0) {
		ploytec_err(&chip->dev->dev, "clear halt on PCM in failed (ret=%d)\n", ret);
		return ret;
	}
	ret = usb_clear_halt(chip->dev, usb_sndintpipe(chip->dev, chip->info->out_ep));
	if (ret < 0)
		ploytec_err(&chip->dev->dev, "clear halt on PCM out failed (ret=%d)\n", ret);
	return ret;
}

/* ========================================================================
 * Device Ops Implementation
 * ======================================================================== */

/*
 * ploytec_init - Perform the Ploytec vendor handshake after USB probe.
 * Allocates private data, reads firmware, sets sample rate, confirms status.
 */
static int ploytec_init(struct ozzy_chip *chip)
{
	struct ploytec_private *priv;
	int ret;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	chip->private_data = priv;

	ploytec_log(&chip->dev->dev, "--- begin handshake sequence ---\n");

	ploytec_log(&chip->dev->dev, "[1/7] reading firmware version...\n");
	ret = ploytec_get_firmware(chip);
	if (ret < 0)
		goto err;

	ploytec_log(&chip->dev->dev, "[2/7] reading hardware status...\n");
	ret = ploytec_get_status(chip);
	if (ret < 0)
		goto err;

	ploytec_log(&chip->dev->dev, "[3/7] reading current sample rate...\n");
	ret = ploytec_get_rate(chip);
	if (ret < 0)
		goto err;

	ploytec_log(&chip->dev->dev, "[4/7] setting sample rate to %u Hz...\n",
		    chip->info->rates[chip->requested_rate]);
	ret = ploytec_set_rate(chip, chip->requested_rate);
	if (ret < 0)
		goto err;

	ploytec_log(&chip->dev->dev, "[5/7] verifying sample rate...\n");
	ret = ploytec_get_rate(chip);
	if (ret > 0)
		ret = -EINVAL;
	if (ret < 0)
		goto err;

	ploytec_log(&chip->dev->dev, "[6/7] reading hardware status...\n");
	ret = ploytec_get_status(chip);
	if (ret < 0)
		goto err;

	ploytec_log(&chip->dev->dev, "[7/7] confirming device status...\n");
	ret = ploytec_confirm_status(chip);
	if (ret < 0)
		goto err;

	ret = ploytec_clear_pcm_halt(chip);
	if (ret < 0)
		goto err;

	ploytec_log(&chip->dev->dev, "--- handshake complete, device ready ---\n");

	return 0;

err:
	ploytec_err(&chip->dev->dev, "handshake failed at step (ret=%d)\n", ret);
	kfree(priv);
	chip->private_data = NULL;
	return ret;
}

/*
 * ploytec_free - Release Ploytec private data.
 */
static void ploytec_free(struct ozzy_chip *chip)
{
	kfree(chip->private_data);
	chip->private_data = NULL;
}

/*
 * ploytec_set_rate - Set the hardware sample rate via vendor requests.
 * Converts the rate to 3 little-endian bytes and sends SET_CUR to both
 * the input (0x86) and output (0x05) endpoints.
 */
static int ploytec_set_rate(struct ozzy_chip *chip, unsigned int rate_index)
{
	struct ploytec_private *priv = chip->private_data;
	unsigned int rate;
	int ret;

	if (rate_index >= chip->info->num_rates)
		return -EINVAL;

	rate = chip->info->rates[rate_index];
	ploytec_encode_rate(rate, priv->xfer_buf);

	ret = usb_control_msg(chip->dev, usb_sndctrlpipe(chip->dev, 0),
			      PLOYTEC_CMD_SET_RATE_REQ, PLOYTEC_CMD_SET_RATE_TYPE,
			      0x0100, PLOYTEC_EP_RATE_IN, priv->xfer_buf, 3, 2000);
	if (ret < 0) return ret;

	ret = usb_control_msg(chip->dev, usb_sndctrlpipe(chip->dev, 0),
			      PLOYTEC_CMD_SET_RATE_REQ, PLOYTEC_CMD_SET_RATE_TYPE,
			      0x0100, PLOYTEC_EP_RATE_OUT, priv->xfer_buf, 3, 2000);
	if (ret < 0) return ret;

	chip->current_rate = rate_index;

	ploytec_log(&chip->dev->dev,
		    "sample rate set: %u Hz (raw: %02X %02X %02X)\n",
		    rate, priv->xfer_buf[0], priv->xfer_buf[1], priv->xfer_buf[2]);

	return 0;
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
	unsigned int f, src_off;

	for (f = 0; f < PLOYTEC_FRAMES_PER_PKT; f++) {
		src_off = dma_off + f * ALSA_FRAME_SIZE;
		if (src_off >= pcm_buffer_size)
			src_off -= pcm_buffer_size;

		ploytec_encode_frame(urb_buf + ploytec_out_frame_offset(layout, f),
				     dma_area + src_off);
	}

	return ALSA_OUT_PKT_SIZE;
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
	unsigned int f;
	unsigned int dst_off;

	for (f = 0; f < PLOYTEC_FRAMES_PER_PKT; f++) {
		dst_off = dma_off + f * ALSA_FRAME_SIZE;
		if (dst_off >= pcm_buffer_size)
			dst_off -= pcm_buffer_size;

		ploytec_decode_frame(dma_area + dst_off,
				     urb_buf + f * PLOYTEC_IN_FRAME_SIZE);
	}

	return ALSA_IN_PKT_SIZE;
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
 * ploytec_fill_midi_out - Embed pending MIDI bytes into a PCM output packet.
 *
 * Rate limiting: MIDI standard baud rate is 31250 bps (~3125 bytes/sec).
 * At 96kHz with 80 frames/packet, we get ~1200 packets/sec. Using all
 * MIDI slots per packet would exceed the MIDI baud rate.
 *
 * We only consume 1 MIDI byte from the first slot, filling all other
 * MIDI bytes with the idle byte (0xFD). Matches the macOS driver's approach.
 */
static void ploytec_fill_midi_out(struct ozzy_chip *chip, uint8_t *urb_buf)
{
	enum ploytec_out_layout layout = ploytec_out_layout(chip);
	struct midi_runtime *rt = chip->midi;
	unsigned int k, midi;

	if (!rt)
		return;

	for (k = 0; k < PLOYTEC_FRAMES_PER_PKT / PLOYTEC_FRAMES_PER_OUT_SUBPKT; k++) {
		midi = ploytec_out_midi_offset(layout, k);
		if (k == 0)
			ozzy_midi_consume(rt, urb_buf + midi, 1, PLOYTEC_MIDI_IDLE_BYTE);
		else
			urb_buf[midi] = PLOYTEC_MIDI_IDLE_BYTE;
		/* BULK: second byte is the 0xFF sync set by init_out_urb; INT: idle MIDI byte */
		if (layout == PLOYTEC_OUT_LAYOUT_INT)
			urb_buf[midi + 1] = PLOYTEC_MIDI_IDLE_BYTE;
	}
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
	.playback_channels     = PLOYTEC_CHANNELS,
	.capture_channels      = PLOYTEC_CHANNELS,
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

/* Xone:DB2: identical to ploytec_info except for the 48 kHz-only rate table */
const struct ozzy_device_info ploytec_db2_info = {
	.name                  = "Ploytec Xone",
	.playback_channels     = PLOYTEC_CHANNELS,
	.capture_channels      = PLOYTEC_CHANNELS,
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
	.rates                 = ploytec_db2_rates,
	.num_rates             = ARRAY_SIZE(ploytec_db2_rates),
	.rates_mask            = SNDRV_PCM_RATE_48000,
	.rate_min              = 48000,
	.rate_max              = 48000,
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
