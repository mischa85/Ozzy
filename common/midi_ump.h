/* SPDX-License-Identifier: MIT */
/*
 * MIDI 1.0 Bytes and Universal MIDI Packets
 *
 * Converts between a MIDI 1.0 byte stream and UMP (Universal MIDI Packet, MIDI 2.0
 * specification), using only UMP's MIDI 1.0 message types, for platforms whose MIDI stack
 * speaks UMP (Windows MIDI Services). A UMP message is 1 to 4 32-bit words; the high nibble of
 * the first word is the message type (MT), the next nibble the group:
 *
 *   MT 0x1  system common and real-time, 1 word:  MT | group | status | data1 | data2
 *   MT 0x2  MIDI 1.0 channel voice, 1 word:        MT | group | status | data1 | data2
 *   MT 0x3  SysEx (7-bit data), 2 words:           MT | group | kind << 4 | count | 6 data bytes
 *
 * SysEx goes in packets of up to 6 data bytes (without F0 and F7): complete in one, or a
 * start, continues and an end. Other message types (MIDI 2.0 channel voice, utility, stream)
 * carry no MIDI 1.0 bytes.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_MIDI_UMP_H
#define OZZY_MIDI_UMP_H

#if defined(__KERNEL__) && defined(__linux__)
#include <linux/types.h>
#else
#include <stdint.h>
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum midi_ump_type {
	MIDI_UMP_UTILITY = 0x0,
	MIDI_UMP_SYSTEM  = 0x1,
	MIDI_UMP_MIDI1   = 0x2,
	MIDI_UMP_SYSEX7  = 0x3,
	MIDI_UMP_MIDI2   = 0x4,
};

/* SysEx packet kinds (status nibble of MT 0x3) */
enum midi_ump_sysex_kind {
	MIDI_UMP_SYSEX_COMPLETE = 0x0,
	MIDI_UMP_SYSEX_START    = 0x1,
	MIDI_UMP_SYSEX_CONTINUE = 0x2,
	MIDI_UMP_SYSEX_END      = 0x3,
};

/* The most words midi_ump_encode_byte writes for one byte: a SysEx end, then a message */
#define MIDI_UMP_ENCODE_MAX_WORDS 3

/*
 * struct midi_ump_encoder - Byte stream to UMP state. Zero-initialise before use. Keeps
 * running status, so the input may use it.
 */
struct midi_ump_encoder {
	uint8_t msg[3];       /* channel or system common message being collected */
	uint8_t len;          /* bytes in msg */
	uint8_t need;         /* message length including status, 0 if no status */
	uint8_t status;       /* running status (channel messages only), 0 if none */
	bool sysex;           /* inside F0 ... F7 */
	bool sysex_started;   /* a start packet went out for this SysEx */
	uint8_t sysex_data[6];
	uint8_t sysex_len;
};

/*
 * midi_ump_encode_byte - Feed one MIDI byte for UMP group @group. Writes the words of the
 * messages it completes to @ump (MIDI_UMP_ENCODE_MAX_WORDS room) and returns how many.
 */
unsigned int midi_ump_encode_byte(struct midi_ump_encoder *enc, uint8_t group, uint8_t byte, uint32_t *ump);

/* Words in the UMP message whose first word is @word0 (by its message type) */
unsigned int midi_ump_words(uint32_t word0);

/*
 * midi_ump_decode - The MIDI 1.0 bytes of the UMP message at @ump (midi_ump_words(ump[0])
 * words) into @out (room for 8). Returns how many; 0 for messages without MIDI 1.0 bytes.
 */
unsigned int midi_ump_decode(const uint32_t *ump, uint8_t out[8]);

#ifdef __cplusplus
}
#endif

#endif /* OZZY_MIDI_UMP_H */
