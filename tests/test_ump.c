/* SPDX-License-Identifier: MIT */
/*
 * common/midi_ump.c: MIDI 1.0 bytes to UMP and back (the Windows driver's MIDI ports). Running
 * status comes back expanded, SysEx of any length round-trips in 6-byte packets (start,
 * continues, end), a status byte inside SysEx ends it, and the words are the specification's.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "common/midi_ump.h"
#include "check.h"
#include <string.h>

/* Encode @in, decode the words; returns the bytes out, and the words in @words */
static unsigned int round_trip(const uint8_t *in, unsigned int len, uint8_t *out, uint32_t *words,
			       unsigned int *nwords)
{
	struct midi_ump_encoder enc;
	uint32_t ump[256];
	unsigned int i, n = 0, w = 0;

	memset(&enc, 0, sizeof(enc));
	for (i = 0; i < len; i++)
		w += midi_ump_encode_byte(&enc, 0, in[i], ump + w);
	for (i = 0; i < w; i += midi_ump_words(ump[i]))
		n += midi_ump_decode(ump + i, out + n);
	if (words)
		memcpy(words, ump, w * sizeof(uint32_t));
	if (nwords)
		*nwords = w;
	return n;
}

int main(void)
{
	uint8_t in[64], out[128];
	uint32_t words[64];
	unsigned int n, w, len;

	/* Channel voice with running status: one MT 2 word per message */
	{
		static const uint8_t notes[] = { 0x90, 0x3C, 0x7F, 0x3E, 0x40, 0xC1, 0x05, 0x06, 0xE0, 0x00, 0x40 };
		static const uint8_t want[] = { 0x90, 0x3C, 0x7F, 0x90, 0x3E, 0x40, 0xC1, 0x05, 0xC1, 0x06, 0xE0, 0x00, 0x40 };

		n = round_trip(notes, sizeof(notes), out, words, &w);
		CHECK(n == sizeof(want) && !memcmp(out, want, n), "running status: %u bytes", n);
		CHECK(w == 5 && words[0] == 0x20903C7F && words[1] == 0x20903E40 && words[2] == 0x20C10500,
		      "channel voice words %08x %08x %08x", words[0], words[1], words[2]);
	}

	/* System messages: MT 1 */
	{
		static const uint8_t sys[] = { 0xF2, 0x10, 0x20, 0xF1, 0x33, 0xF3, 0x05, 0xF6, 0xF8, 0xFA, 0xFE };

		n = round_trip(sys, sizeof(sys), out, words, &w);
		CHECK(n == sizeof(sys) && !memcmp(out, sys, n), "system messages: %u bytes", n);
		CHECK(w == 7 && words[0] == 0x10F21020 && words[4] == 0x10F80000, "system words %08x %08x", words[0],
		      words[4]);
	}

	/* SysEx of every length up to 40 data bytes round-trips; 13 bytes is start, continue, end */
	for (len = 0; len <= 40; len++) {
		unsigned int i;

		in[0] = 0xF0;
		for (i = 0; i < len; i++)
			in[1 + i] = (uint8_t)(i * 5 + 1) & 0x7F;
		in[1 + len] = 0xF7;
		n = round_trip(in, len + 2, out, words, &w);
		CHECK(n == len + 2 && !memcmp(out, in, n), "SysEx of %u bytes: %u back", len, n);
		CHECK(w == 2 * (len <= 6 ? 1 : (len + 5) / 6), "SysEx of %u bytes in %u words", len, w);
		if (len == 13)
			CHECK((words[0] >> 16 & 0xFF) == 0x16 && (words[2] >> 16 & 0xFF) == 0x26 &&
			      (words[4] >> 16 & 0xFF) == 0x31, "13-byte SysEx packets %08x %08x %08x", words[0], words[2],
			      words[4]);
	}

	/* A status byte inside SysEx ends it */
	{
		static const uint8_t cut[] = { 0xF0, 0x01, 0x02, 0x90, 0x3C, 0x7F };
		static const uint8_t want[] = { 0xF0, 0x01, 0x02, 0xF7, 0x90, 0x3C, 0x7F };

		n = round_trip(cut, sizeof(cut), out, NULL, NULL);
		CHECK(n == sizeof(want) && !memcmp(out, want, n), "interrupted SysEx: %u bytes", n);
	}

	/* Messages without MIDI 1.0 bytes decode to nothing; word counts by type */
	{
		static const uint32_t midi2[2] = { 0x40903C00, 0xFFFF0000 };

		CHECK(midi_ump_words(midi2[0]) == 2 && midi_ump_decode(midi2, out) == 0, "MIDI 2.0 note decoded");
		CHECK(midi_ump_words(0xF0000000) == 4 && midi_ump_words(0x00000000) == 1, "word counts");
	}
	return CHECK_DONE("ump");
}
