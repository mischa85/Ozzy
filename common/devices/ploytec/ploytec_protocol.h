/* SPDX-License-Identifier: MIT */
/*
 * Ploytec USB Protocol Helpers
 *
 * Platform-independent protocol logic for Allen & Heath Xone devices
 * using the Ploytec USB audio chipset. These functions contain pure
 * computation -- no I/O, no platform headers, no side effects.
 *
 * Used by both Linux (kernel module) and macOS (kext/HAL) drivers.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_PLOYTEC_PROTOCOL_H
#define OZZY_PLOYTEC_PROTOCOL_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#include <stdbool.h>
#endif

#include "ploytec_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Vendor Request 'I' (0x49) - Hardware Control Registers
 *
 * The 'I' vendor request accesses hardware control registers selected
 * by wIndex. Reads return 1 byte in the data phase; writes encode
 * the value in wValue with no data phase.
 *
 * Reverse-engineered from USB traffic capture and cross-referenced
 * against both the Windows and macOS Ploytec drivers.
 * Some bits are only partially understood -- see per-field comments.
 * ======================================================================== */

/*
 * Register indices for vendor request 'I' (wIndex field).
 */
enum ploytec_ctrl_reg {
	PLOYTEC_REG_AJ_INPUT_SEL    = 0,  /* AJ input selector / digital status */
	PLOYTEC_REG_US3XX_CONFIG    = 1,  /* US3XX / mixer / CPLD config */
	PLOYTEC_REG_DIGITAL_OUT_SEL = 2,  /* digital output selector (write only) */
};

/*
 * AJ Input Selector flags (wIndex=0).
 *
 * WARNING: This is NOT a simple flag register. It is a stateful control
 * byte where many bits encode device-specific composite states. Always
 * use read-modify-write -- never blindly overwrite.
 *
 * Confirmed bits:
 *   0x02 - input routing / source select
 *   0x04 - digital signal lock (detected via (byte & 0x05) == 0x04)
 *
 * Partially understood bits (device-dependent mode/config):
 *   0x01, 0x08, 0x10, 0x20, 0x40, 0x80
 *   Frequently masked/merged, used to encode routing, timing, hw mode.
 */
enum ploytec_aj_input_flags {
	PLOYTEC_AJ_MODE0_BIT        = 0x01,  /* mode/config, device-dependent */
	PLOYTEC_AJ_INPUT_SELECT_BIT = 0x02,  /* input routing / source select */
	PLOYTEC_AJ_DIGITAL_LOCK_BIT = 0x04,  /* digital signal lock */
	PLOYTEC_AJ_MODE3_BIT        = 0x08,  /* mode/config, device-dependent */
	PLOYTEC_AJ_MODE4_BIT        = 0x10,  /* mode/config, device-dependent */
	PLOYTEC_AJ_MODE5_BIT        = 0x20,  /* mode/config, device-dependent */
	PLOYTEC_AJ_MODE6_BIT        = 0x40,  /* mode/config, device-dependent */
	PLOYTEC_AJ_MODE7_BIT        = 0x80,  /* mode/config, device-dependent */
};

/* Digital lock is detected when (byte & LOCK_MASK) == LOCK_VALUE */
#define PLOYTEC_AJ_DIGITAL_LOCK_MASK   0x05
#define PLOYTEC_AJ_DIGITAL_LOCK_VALUE  0x04

/* ESU-specific composite states (observed fixed patterns, not individual flags) */
#define PLOYTEC_AJ_ESU_PRESERVE_MASK   0x4C
#define PLOYTEC_AJ_ESU_STATE_A         0x32
#define PLOYTEC_AJ_ESU_STATE_B         0xB0
#define PLOYTEC_AJ_ESU_STATE_C         0xB2

/*
 * struct ploytec_aj_input_status - Decoded AJ Input Selector register (wIndex=0).
 *
 * Only confirmed fields have meaningful names. The mode bits are exposed
 * individually for logging -- their exact function is device-dependent
 * and not yet fully understood.
 */
struct ploytec_aj_input_status {
	uint8_t raw;            /* raw byte as read from hardware */
	bool input_select;      /* bit 1: input routing / source select (confirmed) */
	bool digital_lock;      /* bit 2: digital signal lock (confirmed) */
	bool mode0;             /* bit 0: device-dependent */
	bool mode3;             /* bit 3: device-dependent */
	bool mode4;             /* bit 4: device-dependent */
	bool mode5;             /* bit 5: device-dependent */
	bool mode6;             /* bit 6: device-dependent */
	bool mode7;             /* bit 7: device-dependent */
};

/*
 * ploytec_decode_aj_input - Decode the AJ Input Selector byte into structured fields.
 * @raw: the byte from vendor request 'I', wIndex=0
 * @out: pointer to struct to populate
 */
static inline void ploytec_decode_aj_input(uint8_t raw, struct ploytec_aj_input_status *out)
{
	out->raw          = raw;
	out->mode0        = (raw & PLOYTEC_AJ_MODE0_BIT)        != 0;
	out->input_select = (raw & PLOYTEC_AJ_INPUT_SELECT_BIT) != 0;
	out->digital_lock = (raw & PLOYTEC_AJ_DIGITAL_LOCK_BIT) != 0;
	out->mode3        = (raw & PLOYTEC_AJ_MODE3_BIT)        != 0;
	out->mode4        = (raw & PLOYTEC_AJ_MODE4_BIT)        != 0;
	out->mode5        = (raw & PLOYTEC_AJ_MODE5_BIT)        != 0;
	out->mode6        = (raw & PLOYTEC_AJ_MODE6_BIT)        != 0;
	out->mode7        = (raw & PLOYTEC_AJ_MODE7_BIT)        != 0;
}

/*
 * ploytec_confirm_wvalue - Compute wValue for the status confirm write-back.
 * @status: raw byte read from vendor request 'I', wIndex=0
 *
 * Sets bit 5 (MODE5/LegacyActive) and sign-extends to 16 bits,
 * matching the official Ploytec driver's (short)(char)byte cast.
 * The high byte is 0xFF when bit 7 is set, 0x00 when clear.
 */
static inline uint16_t ploytec_confirm_wvalue(uint8_t status)
{
	int8_t modified = (int8_t)(status | PLOYTEC_AJ_MODE5_BIT);
	return (uint16_t)(int16_t)modified;
}

/*
 * US3XX Channel Config flags (wIndex=1).
 */
enum ploytec_us3xx_config_flags {
	PLOYTEC_US3XX_OPEN_CPL_SWITCH_N = 0x02,  /* physical switch, active-low */
	PLOYTEC_US3XX_MIXER_WIDTH_BIT   = 0x08,  /* mixer layout: 4ch vs 6ch */
	PLOYTEC_US3XX_MIXER_BYPASS_BIT  = 0x10,  /* set = bypass, clear = active */
};

/* Only bits 3 and 4 are writable; other bits are preserved or forced high */
#define PLOYTEC_US3XX_WRITABLE_MASK    0x18
#define PLOYTEC_US3XX_FORCED_HIGH_MASK 0xE7


/* ========================================================================
 * Packet Layout Tables
 *
 * The Ploytec chipset groups audio frames into sub-packets with MIDI
 * byte slots between them. The layout differs between bulk and
 * interrupt transfer modes. These tables describe where things go
 * so both platforms use identical framing logic.
 * ======================================================================== */

/*
 * struct ploytec_subpacket - Describes one group of audio frames in an output packet.
 * @start_frame: index of the first audio frame in this group
 * @frame_count: number of audio frames in this group
 * @byte_offset: byte offset of first audio frame within the USB packet
 */
struct ploytec_subpacket {
	uint8_t start_frame;
	uint8_t frame_count;
	uint16_t byte_offset;
};

/*
 * struct ploytec_midi_slot - Describes one MIDI byte position in an output packet.
 * @offset:    byte offset within the USB packet
 * @num_bytes: number of MIDI bytes at this position (1 for bulk, 2 for interrupt)
 */
struct ploytec_midi_slot {
	uint16_t offset;
	uint8_t num_bytes;
};

/*
 * Bulk output packet layout (2048 bytes total):
 *   4 groups of 10 frames (480 bytes) + 32-byte gap (1 MIDI + 1 sync + 30 pad)
 *   Group 0: frames  0-9  at offset 0
 *   Group 1: frames 10-19 at offset 512
 *   Group 2: frames 20-29 at offset 1024
 *   Group 3: frames 30-39 at offset 1536
 */
static const struct ploytec_subpacket ploytec_bulk_subpackets[] = {
	{ .start_frame =  0, .frame_count = 10, .byte_offset = 0 },
	{ .start_frame = 10, .frame_count = 10, .byte_offset = 512 },
	{ .start_frame = 20, .frame_count = 10, .byte_offset = 1024 },
	{ .start_frame = 30, .frame_count = 10, .byte_offset = 1536 },
};

/*
 * Interrupt output packet layout (1928 bytes total):
 *   5 groups with 2-byte MIDI gaps between them
 *   Group 0: frames  0-8  (9 frames, 432 bytes) at offset 0
 *   Group 1: frames  9-18 (10 frames) at offset 434
 *   Group 2: frames 19-28 (10 frames) at offset 916
 *   Group 3: frames 29-38 (10 frames) at offset 1398
 *   Group 4: frame  39    (1 frame)   at offset 1880
 */
static const struct ploytec_subpacket ploytec_int_subpackets[] = {
	{ .start_frame =  0, .frame_count =  9, .byte_offset = 0 },
	{ .start_frame =  9, .frame_count = 10, .byte_offset = 434 },
	{ .start_frame = 19, .frame_count = 10, .byte_offset = 916 },
	{ .start_frame = 29, .frame_count = 10, .byte_offset = 1398 },
	{ .start_frame = 39, .frame_count =  1, .byte_offset = 1880 },
};

#define PLOYTEC_BULK_NUM_SUBPACKETS  4
#define PLOYTEC_INT_NUM_SUBPACKETS   5

/* Bulk MIDI slots: 1 byte each, after each 10-frame group */
static const struct ploytec_midi_slot ploytec_bulk_midi_slots[] = {
	{ .offset = 480,  .num_bytes = 1 },
	{ .offset = 992,  .num_bytes = 1 },
	{ .offset = 1504, .num_bytes = 1 },
	{ .offset = 2016, .num_bytes = 1 },
};

/* Interrupt MIDI slots: 2 bytes each, between frame groups */
static const struct ploytec_midi_slot ploytec_int_midi_slots[] = {
	{ .offset = 432,  .num_bytes = 2 },
	{ .offset = 914,  .num_bytes = 2 },
	{ .offset = 1396, .num_bytes = 2 },
	{ .offset = 1878, .num_bytes = 2 },
};

#define PLOYTEC_BULK_NUM_MIDI_SLOTS  4
#define PLOYTEC_INT_NUM_MIDI_SLOTS   4


/* ========================================================================
 * Sample Rate Encoding
 *
 * The Ploytec chipset uses 3-byte little-endian sample rate encoding
 * for SET_CUR / GET_CUR requests.
 * ======================================================================== */

/*
 * ploytec_encode_rate - Encode a sample rate into 3-byte little-endian.
 * @rate: sample rate in Hz (e.g. 44100, 48000, 96000)
 * @buf: output buffer, must be at least 3 bytes
 */
static inline void ploytec_encode_rate(uint32_t rate, uint8_t buf[3])
{
	buf[0] = rate & 0xFF;
	buf[1] = (rate >> 8) & 0xFF;
	buf[2] = (rate >> 16) & 0xFF;
}

/*
 * ploytec_decode_rate - Decode a 3-byte little-endian sample rate.
 * @buf: input buffer with 3 bytes from GET_CUR response
 * Returns the sample rate in Hz.
 */
static inline uint32_t ploytec_decode_rate(const uint8_t buf[3])
{
	return (uint32_t)buf[0] |
	       ((uint32_t)buf[1] << 8) |
	       ((uint32_t)buf[2] << 16);
}


/* ========================================================================
 * Firmware Version
 * ======================================================================== */

struct ploytec_firmware_version {
	uint8_t chip_id;     /* byte[0] of 'V' response */
	uint8_t major;       /* byte[2] / 10 */
	uint8_t minor;       /* byte[2] % 10 */
};

/*
 * ploytec_parse_firmware - Extract version from vendor request 'V' response.
 * @raw: pointer to at least 3 bytes from the firmware version response
 */
static inline struct ploytec_firmware_version ploytec_parse_firmware(const uint8_t *raw)
{
	struct ploytec_firmware_version fw;
	fw.chip_id = raw[0];
	fw.major   = raw[2] / 10;
	fw.minor   = raw[2] % 10;
	return fw;
}

#ifdef __cplusplus
}
#endif

#endif /* OZZY_PLOYTEC_PROTOCOL_H */
