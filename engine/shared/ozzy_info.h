/* SPDX-License-Identifier: MIT */
/*
 * What a driver knows about a device and how it runs it, for ozzy-cli and the ozzy app: one
 * record, the same on every platform. The engine fills it after every handshake
 * (ozzy_engine_publish_info) into the shared memory; each driver hands it out as it is:
 *   - macOS: OzzyHAL's custom property OZZY_PROPERTY_INFO ('ozin'), a CFData of this struct
 *   - Linux: the read-only ALSA bytes control "Ozzy Info"
 *   - Windows: OZZY_KSPROPERTY_INFO (windows/ozzy_ksproperty.h)
 * Only fixed-width fields at their natural alignment, so the kernel and user space of every
 * platform agree on the layout. A reader checks @version and @size and ignores a record it
 * doesn't know.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_INFO_H
#define OZZY_INFO_H

#if defined(__KERNEL__) && defined(__linux__)
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Bump when struct ozzy_info changes */
#define OZZY_INFO_VERSION 1

/* One per enum ozzy_pipe_role (engine/core/ozzy_engine.h), in its order */
enum ozzy_info_pipe_role {
	OZZY_INFO_PIPE_PCM_OUT,
	OZZY_INFO_PIPE_PCM_IN,
	OZZY_INFO_PIPE_MIDI_IN,
	OZZY_INFO_PIPE_MIDI_OUT,
	OZZY_INFO_PIPE_FEEDBACK,
	OZZY_INFO_PIPES,
};

/* The USB connection, as the platform found it */
enum ozzy_usb_speed {
	OZZY_USB_SPEED_UNKNOWN,
	OZZY_USB_SPEED_LOW,
	OZZY_USB_SPEED_FULL,
	OZZY_USB_SPEED_HIGH,
	OZZY_USB_SPEED_SUPER,
};

/* How PCM out goes to the device */
enum ozzy_info_output {
	OZZY_INFO_OUTPUT_NONE,                /* no PCM out (MIDI only) */
	OZZY_INFO_OUTPUT_PLOYTEC_BULK,        /* Ploytec packets in the 512-byte "bulk" layout */
	OZZY_INFO_OUTPUT_PLOYTEC_INTERRUPT,   /* Ploytec packets in the 482-byte "interrupt" layout */
	OZZY_INFO_OUTPUT_ISOC,                /* isochronous, paced by the device's feedback */
};

/* The isochronous feedback the device sends (common/isoc_feedback.h) */
enum ozzy_info_feedback {
	OZZY_INFO_FEEDBACK_NONE,              /* none, or not isochronous */
	OZZY_INFO_FEEDBACK_COUNT,             /* frames per packet */
	OZZY_INFO_FEEDBACK_Q16_16,            /* frames per microframe, 16.16 fixed point */
};

/*
 * struct ozzy_info_pipe - One endpoint the engine runs (address 0: the role is unused).
 * @transfer_type: bmAttributes & 3 (1 isochronous, 2 bulk, 3 interrupt)
 * @interval:      bInterval as the descriptor has it
 * @in_flight:     transfers the engine keeps queued on it
 * @max_packet:    bytes per service interval (packet size times transactions per microframe)
 */
struct ozzy_info_pipe {
	uint8_t address;
	uint8_t transfer_type;
	uint8_t interval;
	uint8_t in_flight;
	uint16_t max_packet;
	uint16_t reserved;
};

/*
 * struct ozzy_info - The device and its stream, as of the last handshake.
 * @starts:          handshakes since the driver took the device (each start, rate change and
 *                   device setting is one)
 * @family/@status:  enum ozzy_family / enum ozzy_status of its ozzy_devices.def row
 * @usb_speed:       enum ozzy_usb_speed
 * @interfaces:      the claimed interfaces (bInterfaceNumber), @interface_count of them, each
 *                   at @alt_setting
 * @output:          enum ozzy_info_output
 * @feedback:        enum ozzy_info_feedback
 * @firmware_valid:  @firmware (a version, three parts: 1.4.1) and @firmware_chip (a chip id)
 *                   are the device's answer to the firmware request (Ploytec 'V')
 * @input_selector_valid: @input_selector is the input selector byte the handshake read
 *                   (Ploytec 'A' request, AjExtData)
 * @sample_rate:     the rate the device runs at (Hz); @boot_rate what it ran at before the
 *                   driver set it (0: not read); @lock_rate the S/PDIF rate while @clock_locked
 * @device_rates:    OZZY_RATE_* bits: what the device lists; @host_rates: what a host may pick
 *                   now (only the lock rate while locked to S/PDIF)
 * @transfer_frames: the most frames one PCM transfer moves, by enum ozzy_pcm_dir (out, in)
 * @out_bytes_per_frame/@in_bytes_per_frame: bytes one frame takes on the wire
 * @safety_offset_frames: how far ahead of the device's clock the host must write output (the
 *                   transfers queued, at least the jitter floor, OZZY_SAFETY_MIN_US)
 * @input_safety_offset_frames: how far behind the clock the host may read input (the same way)
 * @timestamp_frames: frames between the clock anchors the engine publishes
 * @driver_version:  the commit the driver was built from (common/ozzy_version.h), terminated
 */
struct ozzy_info {
	uint32_t version;
	uint32_t size;
	uint32_t starts;
	uint16_t vendor_id;
	uint16_t product_id;

	uint8_t family;
	uint8_t status;
	uint8_t usb_speed;
	uint8_t alt_setting;
	uint8_t interface_count;
	uint8_t interfaces[4];
	uint8_t output;
	uint8_t feedback;
	uint8_t firmware_valid;
	uint8_t firmware_chip;
	uint8_t firmware[3];
	uint8_t input_selector_valid;
	uint8_t input_selector;
	uint8_t clock_locked;
	uint8_t reserved;

	uint32_t sample_rate;
	uint32_t boot_rate;
	uint32_t lock_rate;
	uint32_t device_rates;
	uint32_t host_rates;
	uint32_t in_channels;
	uint32_t out_channels;
	uint32_t transfer_frames[2];
	uint32_t out_bytes_per_frame;
	uint32_t in_bytes_per_frame;
	uint32_t safety_offset_frames;
	uint32_t input_safety_offset_frames;
	uint32_t timestamp_frames;
	char driver_version[40];

	struct ozzy_info_pipe pipes[OZZY_INFO_PIPES];
};

#ifdef __cplusplus
}
#endif

#endif /* OZZY_INFO_H */
