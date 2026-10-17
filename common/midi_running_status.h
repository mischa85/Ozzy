/* SPDX-License-Identifier: MIT */
/*
 * Running status expansion for a MIDI byte stream going to a device.
 *
 * The ALSA sequencer's rawmidi bridge drops a status byte equal to the previous one
 * (seq_midi.c never calls snd_midi_event_no_status()). The official drivers only ever send
 * complete messages (CoreMIDI packets carry no running status), and at least one Ploytec
 * firmware ignores running status (PR #80), so a producer whose source may use running
 * status puts the status back in front of every channel message.
 *
 * Pure C with no platform dependencies.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_MIDI_RUNNING_STATUS_H
#define OZZY_MIDI_RUNNING_STATUS_H

#if defined(__KERNEL__) && defined(__linux__)
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * struct midi_running_status - The expander's state, zero to start.
 * @status:    last channel status byte, 0 = none
 * @remaining: data bytes left in the current message
 */
struct midi_running_status {
	uint8_t status;
	uint8_t remaining;
};

/* Data bytes that follow a channel status byte (0x80..0xEF) */
static inline uint8_t midi_running_status_data_len(uint8_t status)
{
	return ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0) ? 1 : 2;
}

/*
 * midi_running_status_expand - One byte of the stream: writes it to @out, with the running
 * status in front when it starts a message that left the status out. Returns how many bytes
 * it wrote (1 or 2).
 */
static inline unsigned int midi_running_status_expand(struct midi_running_status *rs, uint8_t byte,
						      uint8_t out[2])
{
	unsigned int n = 0;

	if (byte >= 0xF8) {
		/* real-time: may appear anywhere, doesn't affect running status */
	} else if (byte >= 0xF0) {
		/* SysEx and system common cancel running status; their data passes as is */
		rs->status = 0;
		rs->remaining = 0;
	} else if (byte & 0x80) {
		rs->status = byte;
		rs->remaining = midi_running_status_data_len(byte);
	} else {
		if (rs->status && !rs->remaining) {
			out[n++] = rs->status;
			rs->remaining = midi_running_status_data_len(rs->status);
		}
		if (rs->remaining)
			rs->remaining--;
	}
	out[n++] = byte;
	return n;
}

#ifdef __cplusplus
}
#endif

#endif /* OZZY_MIDI_RUNNING_STATUS_H */
