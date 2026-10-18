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

#if defined(__KERNEL__) && defined(__linux__)
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
 *   0x02 - clock source: set = automatic (S/PDIF in when valid), clear = internal
 *   0x04 - digital signal lock (detected via (byte & 0x05) == 0x04)
 *
 * Partially understood bits (device-dependent mode/config):
 *   0x01, 0x08, 0x10, 0x20, 0x40, 0x80
 *   Frequently masked/merged, used to encode routing, timing, hw mode.
 */
enum ploytec_aj_input_flags {
	PLOYTEC_AJ_MODE0_BIT        = 0x01,  /* mode/config, device-dependent */
	PLOYTEC_AJ_INPUT_SELECT_BIT = 0x02,  /* clock source: set = automatic (ploytec_aj_clock_byte) */
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

/*
 * ploytec_aj_clock_byte - The selector byte @b with bit 1 for clock source @clock
 * (enum ozzy_clock_source). The official updateAjInputSelector sets bit 1 for
 * ClockSource (AjExtData[1]) 0 and clears it otherwise; Tascam's Control Panel calls 0
 * "Automatic" (the default) and 1 "Internal". Not yet seen on a device.
 */
static inline uint8_t ploytec_aj_clock_byte(uint8_t b, uint8_t clock)
{
	return clock == 0 ? (uint8_t)(b | PLOYTEC_AJ_INPUT_SELECT_BIT) :
			    (uint8_t)(b & ~PLOYTEC_AJ_INPUT_SELECT_BIT);
}

/*
 * ploytec_aj_locked - Whether the selector byte reports digital lock.
 */
static inline bool ploytec_aj_locked(uint8_t b)
{
	return (b & PLOYTEC_AJ_DIGITAL_LOCK_MASK) == PLOYTEC_AJ_DIGITAL_LOCK_VALUE;
}

/*
 * ESI U-series selector states (updateAjInputSelector_Esu186): the byte keeps the
 * PRESERVE_MASK bits it reads and gets one of three states, from the driver's
 * clock setting (AjExtData+4): 2 -> STATE_A, 1 -> STATE_B, else STATE_C. STATE_B
 * also sends the 'A' request with bit 0 set. Which state is which clock source
 * ("Internal (Analoge audio only)" / "External SPDIF" in ESI's 2.0 driver) is open;
 * STATE_A is the one the driver reports as its default (*param_1 = 0x32).
 */
#define PLOYTEC_AJ_ESU_PRESERVE_MASK   0x4C
#define PLOYTEC_AJ_ESU_STATE_A         0x32
#define PLOYTEC_AJ_ESU_STATE_B         0xB0
#define PLOYTEC_AJ_ESU_STATE_C         0xB2

/* ESI CPLD byte ('I' wIndex 1, setEsuCpldByte): wValue = setting | 0xE7, setting 0 at start */
#define PLOYTEC_ESI_CPLD_BASE          0xE7

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
 * The official setAJDMAInputChannels (bulkAudioRun, every stream start): sets
 * bit 5 (16-lane input DMA, for up to 16 inputs, which every Ozzy device has) and
 * bit 4 (MODE4/StreamingArmed; the Xones already report it at power-up, so for
 * them only bit 5 changes), and sign-extends to 16 bits,
 * matching the official Ploytec driver's (short)(char)byte cast.
 * The high byte is 0xFF when bit 7 is set, 0x00 when clear.
 */
static inline uint16_t ploytec_confirm_wvalue(uint8_t status)
{
	int8_t modified = (int8_t)(status | PLOYTEC_AJ_MODE5_BIT | PLOYTEC_AJ_MODE4_BIT);
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
 * Output Packet Layout
 *
 * Output goes in sub-packets of 10 frames, with a MIDI byte and a control
 * byte embedded in each; a transfer is any number of them in a row.
 * There are two layouts. Which one a device uses follows the PCM OUT
 * endpoint's wMaxPacketSize, not its transfer type, like the official
 * driver (AudioBulkIRP::useOut):
 *
 *   BULK (wMaxPacketSize <= 512), 512 bytes per sub-packet:
 *     [480 PCM (10 frames)][MIDI][control][30 pad]
 *   INT (wMaxPacketSize 964), 482 bytes per sub-packet, 2 per USB packet:
 *     [432 PCM (9 frames)][MIDI][control][48 PCM (1 frame)]
 *
 * ploytec_midi.h fills the MIDI slots.
 *
 * Both platforms compute all offsets with the helpers below.
 * ======================================================================== */

enum ploytec_out_layout {
	PLOYTEC_OUT_LAYOUT_BULK,
	PLOYTEC_OUT_LAYOUT_INT,
};

/*
 * ploytec_out_layout_for_maxpacket - Select the output layout from the
 * PCM OUT endpoint's wMaxPacketSize.
 */
static inline enum ploytec_out_layout ploytec_out_layout_for_maxpacket(uint16_t max_packet_size)
{
	return max_packet_size <= PLOYTEC_BULK_OUT_SUBPKT_SIZE ? PLOYTEC_OUT_LAYOUT_BULK
							      : PLOYTEC_OUT_LAYOUT_INT;
}

/* Bytes of one output sub-packet (10 frames, a MIDI slot and a control byte) */
static inline uint32_t ploytec_out_subpkt_size(enum ploytec_out_layout layout)
{
	return layout == PLOYTEC_OUT_LAYOUT_BULK ? PLOYTEC_BULK_OUT_SUBPKT_SIZE : PLOYTEC_INT_OUT_SUBPKT_SIZE;
}

/*
 * ploytec_out_frame_offset - Byte offset of frame @frame of a run of sub-packets from the start
 * of its first (frame 10 is the first of the second sub-packet).
 */
static inline uint32_t ploytec_out_frame_offset(enum ploytec_out_layout layout, uint32_t frame)
{
	uint32_t subpkt = frame / PLOYTEC_FRAMES_PER_OUT_SUBPKT;
	uint32_t index = frame % PLOYTEC_FRAMES_PER_OUT_SUBPKT;

	if (layout == PLOYTEC_OUT_LAYOUT_BULK)
		return subpkt * PLOYTEC_BULK_OUT_SUBPKT_SIZE + index * PLOYTEC_OUT_FRAME_SIZE;

	/* INT: frames 0-8, then the MIDI slot and control byte, then frame 9 */
	return subpkt * PLOYTEC_INT_OUT_SUBPKT_SIZE + index * PLOYTEC_OUT_FRAME_SIZE +
	       (index == PLOYTEC_FRAMES_PER_OUT_SUBPKT - 1 ? 2 : 0);
}

/*
 * ploytec_out_midi_offset - Byte offset of the MIDI slot of sub-packet @subpkt of a run.
 * The control byte follows it.
 */
static inline uint32_t ploytec_out_midi_offset(enum ploytec_out_layout layout, uint32_t subpkt)
{
	if (layout == PLOYTEC_OUT_LAYOUT_BULK)
		return subpkt * PLOYTEC_BULK_OUT_SUBPKT_SIZE +
		       PLOYTEC_FRAMES_PER_OUT_SUBPKT * PLOYTEC_OUT_FRAME_SIZE;

	return subpkt * PLOYTEC_INT_OUT_SUBPKT_SIZE +
	       (PLOYTEC_FRAMES_PER_OUT_SUBPKT - 1) * PLOYTEC_OUT_FRAME_SIZE;
}

/*
 * ploytec_out_init_subpkts - Fill @count sub-packets from @pkt with silence, idle MIDI slots
 * and idle control bytes.
 */
static inline void ploytec_out_init_subpkts(enum ploytec_out_layout layout, uint8_t *pkt, uint32_t count)
{
	uint32_t k, midi;

	for (k = 0; k < count * ploytec_out_subpkt_size(layout); k++)
		pkt[k] = 0;

	for (k = 0; k < count; k++) {
		midi = ploytec_out_midi_offset(layout, k);
		pkt[midi] = PLOYTEC_MIDI_IDLE_BYTE;
		pkt[midi + 1] = PLOYTEC_MIDI_CONTROL_IDLE;
	}
}

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
