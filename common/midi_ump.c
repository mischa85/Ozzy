/* SPDX-License-Identifier: MIT */
/*
 * MIDI 1.0 Bytes and Universal MIDI Packets
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "midi_ump.h"

/* Message length including status for 0x8n-0xEn, indexed by the high nibble */
static const uint8_t channel_msg_length[16] = {
	0, 0, 0, 0, 0, 0, 0, 0, 3, 3, 3, 3, 2, 2, 3, 0
};

/* Words per message type (UMP 1.1, table 4) */
static const uint8_t type_words[16] = {
	1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4
};

static uint32_t word32(uint8_t type, uint8_t group, uint8_t b1, uint8_t b2, uint8_t b3)
{
	return (uint32_t)type << 28 | (uint32_t)(group & 0x0F) << 24 | (uint32_t)b1 << 16 | (uint32_t)b2 << 8 | b3;
}

/* A SysEx packet of the collected data bytes; they are cleared */
static unsigned int put_sysex(struct midi_ump_encoder *enc, uint8_t group, enum midi_ump_sysex_kind kind,
			      uint32_t *ump)
{
	const uint8_t *d = enc->sysex_data;
	uint8_t b[6] = { 0 };
	unsigned int i;

	for (i = 0; i < enc->sysex_len; i++)
		b[i] = d[i];
	ump[0] = word32(MIDI_UMP_SYSEX7, group, (uint8_t)(kind << 4 | enc->sysex_len), b[0], b[1]);
	ump[1] = (uint32_t)b[2] << 24 | (uint32_t)b[3] << 16 | (uint32_t)b[4] << 8 | b[5];
	enc->sysex_len = 0;
	return 2;
}

/* End the SysEx in progress: an end packet, or a complete one if nothing went out yet */
static unsigned int end_sysex(struct midi_ump_encoder *enc, uint8_t group, uint32_t *ump)
{
	unsigned int n = put_sysex(enc, group, enc->sysex_started ? MIDI_UMP_SYSEX_END : MIDI_UMP_SYSEX_COMPLETE, ump);

	enc->sysex = false;
	enc->sysex_started = false;
	return n;
}

/* The word of the complete message in enc->msg */
static uint32_t put_message(const struct midi_ump_encoder *enc, uint8_t group)
{
	uint8_t status = enc->msg[0];

	return word32(status < 0xF0 ? MIDI_UMP_MIDI1 : MIDI_UMP_SYSTEM, group, status,
		      enc->len > 1 ? enc->msg[1] : 0, enc->len > 2 ? enc->msg[2] : 0);
}

unsigned int midi_ump_encode_byte(struct midi_ump_encoder *enc, uint8_t group, uint8_t byte, uint32_t *ump)
{
	unsigned int n = 0;

	/* Real-time messages may appear anywhere, even inside SysEx, and change no state */
	if (byte >= 0xF8) {
		ump[0] = word32(MIDI_UMP_SYSTEM, group, byte, 0, 0);
		return 1;
	}

	if (byte == 0xF7)
		return enc->sysex ? end_sysex(enc, group, ump) : 0;

	if (byte >= 0x80) {
		/* Any other status byte ends SysEx (the F7 got lost) and running status */
		if (enc->sysex)
			n = end_sysex(enc, group, ump);
		enc->status = 0;
		enc->len = 0;
		enc->need = 0;

		if (byte == 0xF0) {
			enc->sysex = true;
			enc->sysex_started = false;
			enc->sysex_len = 0;
			return n;
		}
		if (byte < 0xF0) {
			enc->status = byte;
			enc->need = channel_msg_length[byte >> 4];
		} else if (byte == 0xF2) {
			enc->need = 3;
		} else if (byte == 0xF1 || byte == 0xF3) {
			enc->need = 2;
		} else if (byte == 0xF6) {
			enc->need = 1;
		} else {
			return n;                   /* F4, F5: undefined */
		}
		enc->msg[enc->len++] = byte;
		if (enc->len < enc->need)
			return n;
		ump[n++] = put_message(enc, group);
		enc->len = 0;
		enc->need = 0;
		return n;
	}

	/* Data byte */
	if (enc->sysex) {
		/* A full packet goes out once the next byte shows the SysEx goes on */
		if (enc->sysex_len == 6) {
			n = put_sysex(enc, group, enc->sysex_started ? MIDI_UMP_SYSEX_CONTINUE : MIDI_UMP_SYSEX_START,
				      ump);
			enc->sysex_started = true;
		}
		enc->sysex_data[enc->sysex_len++] = byte;
		return n;
	}

	if (enc->len == 0) {
		if (!enc->status)
			return 0;                   /* no status to attach it to */
		enc->msg[enc->len++] = enc->status;  /* running status */
		enc->need = channel_msg_length[enc->status >> 4];
	}
	enc->msg[enc->len++] = byte;
	if (enc->len < enc->need)
		return 0;
	ump[0] = put_message(enc, group);
	enc->len = 0;
	return 1;
}

unsigned int midi_ump_words(uint32_t word0)
{
	return type_words[word0 >> 28];
}

unsigned int midi_ump_decode(const uint32_t *ump, uint8_t out[8])
{
	uint8_t status = (uint8_t)(ump[0] >> 16);
	unsigned int n = 0, count, i;

	switch (ump[0] >> 28) {
	case MIDI_UMP_SYSTEM:
		out[0] = status;
		if (status == 0xF2)
			n = 3;
		else if (status == 0xF1 || status == 0xF3)
			n = 2;
		else if (status == 0xF6 || status >= 0xF8)
			n = 1;
		break;
	case MIDI_UMP_MIDI1:
		out[0] = status;
		n = channel_msg_length[status >> 4];
		break;
	case MIDI_UMP_SYSEX7: {
		uint8_t kind = status >> 4;
		const uint8_t d[6] = { (uint8_t)(ump[0] >> 8), (uint8_t)ump[0], (uint8_t)(ump[1] >> 24),
				       (uint8_t)(ump[1] >> 16), (uint8_t)(ump[1] >> 8), (uint8_t)ump[1] };

		count = status & 0x0F;
		if (count > 6)
			return 0;
		if (kind == MIDI_UMP_SYSEX_COMPLETE || kind == MIDI_UMP_SYSEX_START)
			out[n++] = 0xF0;
		for (i = 0; i < count; i++)
			out[n++] = d[i] & 0x7F;
		if (kind == MIDI_UMP_SYSEX_COMPLETE || kind == MIDI_UMP_SYSEX_END)
			out[n++] = 0xF7;
		return n;
	}
	default:
		return 0;
	}
	for (i = 1; i < n; i++)
		out[i] = (uint8_t)(ump[0] >> (16 - 8 * i)) & 0x7F;
	return n;
}
