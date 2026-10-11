/* SPDX-License-Identifier: MIT */
/*
 * Ozzy USB Audio Driver - Core Definitions
 *
 * Device-agnostic abstraction layer for non-class-compliant USB audio
 * hardware. Each device family implements ozzy_device_info (static
 * hardware descriptor) and ozzy_device_ops (behavioral contract).
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_H
#define OZZY_H

#include <linux/usb.h>
#include <sound/core.h>
#include <sound/pcm.h>

struct ozzy_chip;
struct pcm_runtime;
struct midi_runtime;

/*
 * struct ozzy_device_info - Static hardware descriptor for a device family.
 *
 * One const instance per device family (or per model if they differ).
 * Describes the hardware's capabilities and topology. Never changes
 * at runtime. Populated in each device file (e.g. devices/ploytec.c).
 */
struct ozzy_device_info {
	const char *name;                 /* human-readable name, e.g. "Ploytec Xone" */

	/* PCM topology */
	unsigned int playback_channels;   /* number of playback channels (e.g. 8) */
	unsigned int capture_channels;    /* number of capture channels (e.g. 8) */
	unsigned int out_packet_size;     /* default output URB size in bytes */
	unsigned int in_packet_size;      /* input URB size in bytes */
	unsigned int frames_per_out_packet; /* audio frames per output USB transfer */
	unsigned int frames_per_in_packet;  /* audio frames per input USB transfer */
	unsigned int out_ep;              /* output endpoint number (e.g. 5) */
	unsigned int in_ep;               /* input endpoint number (e.g. 6) */
	u64 alsa_format;                  /* SNDRV_PCM_FMTBIT_xxx */
	unsigned int bytes_per_sample;    /* bytes per ALSA sample (e.g. 3 for S24_3LE) */

	/* MIDI topology */
	unsigned int midi_in_ep;          /* dedicated MIDI input endpoint (0 = none) */
	bool midi_out_embedded;           /* true if MIDI out is embedded in PCM out packets */

	/* USB interface configuration */
	unsigned int num_interfaces;      /* number of USB interfaces to claim (e.g. 2) */
	unsigned int alt_setting;         /* alternate setting to select (e.g. 1) */

	/* Sample rates */
	const unsigned int *rates;        /* array of supported rates (e.g. {44100, 48000, ...}) */
	unsigned int num_rates;           /* number of entries in rates[] */
	unsigned int rates_mask;          /* SNDRV_PCM_RATE_xxx bitmask */
	unsigned int rate_min;            /* lowest supported rate */
	unsigned int rate_max;            /* highest supported rate */
};

/*
 * struct ozzy_device_ops - Behavioral contract for a device family.
 *
 * Function pointer table implemented by each device family.
 * Follows the Linux kernel convention (cf. struct file_operations,
 * struct snd_pcm_ops). NULL pointers mean "not supported" or
 * "use default behavior" as documented per callback.
 */
struct ozzy_device_ops {
	/*
	 * init - Perform device-specific initialization after USB probe.
	 * Called once after the chip struct is populated. Should perform the
	 * vendor handshake sequence (firmware version, status, sample rate).
	 * Allocate chip->private_data here if needed.
	 * Returns 0 on success, negative errno on failure.
	 */
	int (*init)(struct ozzy_chip *chip);

	/*
	 * free - Release device-specific resources.
	 * Called during card destruction. Free chip->private_data here.
	 */
	void (*free)(struct ozzy_chip *chip);

	/*
	 * reset - Perform a USB device reset (e.g. for sample rate changes).
	 * Called when the rate change requires a full device reset.
	 * The core's pre_reset/post_reset callbacks handle URB teardown/reinit.
	 * Returns 0 on success, negative errno on failure.
	 */
	int (*reset)(struct ozzy_chip *chip);

	/*
	 * process_out_packet - Encode ALSA audio into a device output USB packet.
	 * Zero-copy: reads directly from the ALSA DMA area, writes directly
	 * into the URB buffer. Handles device-specific framing (padding, gaps).
	 * Must handle DMA ring buffer wraparound when dma_off + packet > buf_size.
	 * @urb_buf:         destination URB buffer (device format)
	 * @dma_area:        source ALSA DMA ring buffer (interleaved samples)
	 * @dma_off:         current byte offset in the DMA ring buffer
	 * @pcm_buffer_size: total DMA ring buffer size in bytes
	 * Returns: number of ALSA bytes consumed from the DMA area.
	 */
	unsigned int (*process_out_packet)(struct ozzy_chip *chip, uint8_t *urb_buf,
					   uint8_t *dma_area, unsigned int dma_off,
					   unsigned int pcm_buffer_size);

	/*
	 * process_in_packet - Decode a device input USB packet into ALSA audio.
	 * Zero-copy: reads directly from the URB buffer, writes directly into
	 * the ALSA DMA area. Must handle DMA ring buffer wraparound.
	 * @urb_buf:         source URB buffer (device format)
	 * @dma_area:        destination ALSA DMA ring buffer (interleaved samples)
	 * @dma_off:         current byte offset in the DMA ring buffer
	 * @pcm_buffer_size: total DMA ring buffer size in bytes
	 * Returns: number of ALSA bytes written to the DMA area.
	 */
	unsigned int (*process_in_packet)(struct ozzy_chip *chip, uint8_t *urb_buf,
					  uint8_t *dma_area, unsigned int dma_off,
					  unsigned int pcm_buffer_size);

	/*
	 * init_out_urb - Fill an output URB buffer with the initial silence pattern.
	 * Called once per URB at allocation time. Should write device-specific
	 * silence values, sync bytes, padding, and MIDI idle bytes.
	 * @buffer: the URB buffer to initialize (size from get_out_packet_size)
	 */
	void (*init_out_urb)(struct ozzy_chip *chip, uint8_t *buffer);

	/*
	 * fill_midi_out - Embed pending MIDI output bytes into a PCM out packet.
	 * Called from the out URB handler after audio processing. Reads from
	 * the MIDI runtime's send buffer and writes into device-specific byte
	 * positions within the URB buffer.
	 * NULL if device doesn't embed MIDI in PCM output packets.
	 * @urb_buf: the output URB buffer to write MIDI bytes into
	 */
	void (*fill_midi_out)(struct ozzy_chip *chip, uint8_t *urb_buf);

	/*
	 * get_out_packet_size - Return the output packet size in bytes.
	 * May differ between bulk and interrupt transfer modes.
	 * Called during URB allocation to determine buffer size.
	 * If NULL, the core uses info->out_packet_size.
	 * @is_bulk: true if the output endpoint uses bulk transfers
	 */
	unsigned int (*get_out_packet_size)(struct ozzy_chip *chip, bool is_bulk);
};

/*
 * struct ozzy_device_desc - Pairs a device's info and ops for USB ID lookup.
 *
 * Stored in usb_device_id.driver_data so the probe function can look up
 * both the hardware descriptor and behavioral contract from the USB match.
 */
struct ozzy_device_desc {
	const struct ozzy_device_info *info;
	const struct ozzy_device_ops *ops;
};

/*
 * struct ozzy_chip - Per-device instance state.
 *
 * One allocated per USB device that is plugged in. Stored as
 * snd_card->private_data. Contains pointers to the matched device
 * info/ops and all runtime subsystem state.
 */
struct ozzy_chip {
	struct usb_device *dev;           /* USB device handle */
	struct snd_card *card;            /* ALSA sound card */

	const struct ozzy_device_info *info; /* static hardware descriptor */
	const struct ozzy_device_ops *ops;   /* device behavior callbacks */

	struct pcm_runtime *pcm;          /* PCM subsystem runtime (NULL until init) */
	struct midi_runtime *midi;        /* MIDI subsystem runtime (NULL until init) */

	unsigned int current_rate;        /* rate index currently active on hardware */
	unsigned int requested_rate;      /* rate index requested by ALSA */
	bool is_bulk;                     /* true if output EP uses bulk transfers */
	uint16_t out_maxpacket;           /* output EP wMaxPacketSize */

	struct usb_interface *intf;       /* primary USB interface */
	struct usb_interface *intf2;      /* secondary USB interface (NULL if single) */

	void *private_data;               /* device-specific runtime data */
};

#endif /* OZZY_H */
