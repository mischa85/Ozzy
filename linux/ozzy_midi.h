/* SPDX-License-Identifier: MIT */
/*
 * Ozzy USB Audio Driver - MIDI Subsystem
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_MIDI_H
#define OZZY_MIDI_H

#include <sound/rawmidi.h>

struct ozzy_chip;

#define OZZY_MIDI_N_URBS          4
#define OZZY_MIDI_PACKET_SIZE     512
#define OZZY_MIDI_SEND_BUF_SIZE   512   /* power of two (ring index mask) */
#define OZZY_MIDI_IN_MAX_BYTES    16    /* MIDI bytes per input transfer after decode_midi_in */
#define OZZY_MIDI_OUT_XFER_MAX    16    /* bytes per dedicated MIDI OUT transfer */

struct midi_urb {
	struct ozzy_chip *chip;
	struct urb instance;
	struct usb_anchor submitted;
	u8 *buffer;
};

/*
 * struct midi_runtime - Per-device MIDI subsystem state.
 *
 * All MIDI state is per-instance (no globals). The send ring holds
 * pending MIDI output bytes that are consumed by the device's
 * fill_midi_out callback during PCM output URB handling. Both sides
 * hold out_lock.
 */
struct midi_runtime {
	struct ozzy_chip *chip;
	struct snd_rawmidi *instance;

	struct snd_rawmidi_substream *in;   /* active input substream (NULL if closed) */
	struct snd_rawmidi_substream *out;  /* active output substream (NULL if closed) */

	bool active;

	spinlock_t in_lock;
	spinlock_t out_lock;

	struct midi_urb midi_in_urbs[OZZY_MIDI_N_URBS];

	/* MIDI output send ring, filled from the rawmidi output substream */
	u8 *send_buffer;                    /* pending MIDI bytes to embed in PCM out */
	unsigned int send_head;             /* write position */
	unsigned int send_tail;             /* read position */

	/* Running status expansion of the outgoing stream (see ozzy_midi_put_out) */
	u8 run_status;                      /* last channel status byte, 0 = none */
	u8 run_remaining;                   /* data bytes left in the current message */
};

/*
 * ozzy_midi_init - Create and initialize the MIDI subsystem.
 * Allocates midi_runtime, registers ALSA rawmidi, sets up URBs.
 * Returns 0 on success, negative errno on failure.
 */
int ozzy_midi_init(struct ozzy_chip *chip);

/*
 * ozzy_midi_init_urbs - (Re)initialize and submit MIDI input URBs.
 * Called during initial probe and after USB device reset.
 * Returns 0 on success, negative errno on failure.
 */
int ozzy_midi_init_urbs(struct ozzy_chip *chip);

/*
 * ozzy_midi_abort - Stop all MIDI URBs immediately.
 * Called during disconnect and pre-reset.
 */
void ozzy_midi_abort(struct ozzy_chip *chip);

/*
 * ozzy_midi_consume - Read pending MIDI output bytes from the send buffer.
 * Called by the device's fill_midi_out callback to get bytes for embedding
 * in PCM output packets. Fills the output with MIDI idle bytes (0xFD from
 * the device's defs) when no data is pending.
 * @buffer: destination buffer to write MIDI bytes into
 * @count:  number of bytes to consume
 * @idle_byte: byte value to use when no MIDI data is pending
 */
void ozzy_midi_consume(struct midi_runtime *rt, u8 *buffer, int count,
		       u8 idle_byte);

#endif /* OZZY_MIDI_H */
