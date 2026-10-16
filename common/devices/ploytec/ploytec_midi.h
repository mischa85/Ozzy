/* SPDX-License-Identifier: MIT */
/*
 * Ploytec MIDI Framing
 *
 * MIDI travels in both directions in frames the device times by its sample
 * clock, so how much fits depends on the sample rate. Everything that depends
 * on it is set up once per stream in struct ploytec_midi, which both
 * platforms use:
 *
 * MIDI out is embedded in the PCM output packets. Every output sub-packet
 * (10 frames) has a MIDI slot followed by a control byte. A slot may carry
 * one byte, but filling every slot would be 9600 bytes/s at 96 kHz, three
 * times what a MIDI cable carries (31250 baud, 10 bits per byte = 3125
 * bytes/s). The official driver paces the slots so that 2500 bytes/s (80%)
 * get through at any rate (USBMidiPattern::initForBulk builds a table with an
 * error accumulator; PGDevice::rtsProcessBulkOut consults it per sub-packet).
 * Ozzy runs the same accumulator without the table. Over any stretch of time
 * T it sends at most 2500 * T + 1 bytes, so it is never more than one byte
 * ahead of the cable, and only within 1.6 ms (where 625 * T < 1).
 *
 * The control byte after each slot is 0xFF, which the official driver sends
 * when its control queue is empty (otherwise control | 0x18; what fills that
 * queue is unknown).
 *
 * MIDI in arrives on its own endpoint, one frame per transfer: a few MIDI
 * byte slots, PLOYTEC_MIDI_IDLE_BYTE in the empty ones, at some rates followed
 * by a control byte that is not MIDI. The slot count depends on the rate and
 * the PCM IN endpoint, as the official driver selects them
 * (AJ::streamingStarted: inACR_M4_C1 / inACR_M1_C1 / inACR_M2):
 *
 *   <= 48 kHz                               4 slots + control byte
 *   > 48 kHz, PCM IN interrupt, 1024 bytes  2 slots
 *   > 48 kHz, otherwise                     1 slot + control byte
 *
 * DB4 fw 1.4.1 sends "FD FD FD B0 FF" at 48 kHz and "FD B0" at 96 kHz.
 *
 * Bulk in / isoc out devices send MIDI out on its own endpoint instead of in
 * the PCM packets, in transfers of a fixed size: MIDI bytes first, 0xFD padding,
 * a control byte last (AJ::fireOutputInternal). MIDI IN transfers have the same
 * size. Two framings, picked per device by the official driver's kInACR
 * (AJ::configurationDone, AJ::streamingStarted):
 *
 *   M4_C1 (kInACR 2, 3, and the first port of the MPC's two-port frame):
 *     9-byte transfers; MIDI IN: 4 slots at every rate (inACR_M4_C1); MIDI out:
 *     4 or 3 bytes per transfer (pattern 4,3,3,3,3,3,3,3). One transfer per
 *     millisecond is 3125 bytes/s, a 31250-baud cable.
 *   NS7 (kInACR 8, Numark NS7/V7/NS6/4Trak):
 *     42-byte transfers (0x2a); MIDI IN: all 41 bytes before the control byte
 *     (inACR_NS7); MIDI out: the official driver allows up to 39 bytes per
 *     transfer (initNS7 fills the pattern with 0x27). Ozzy keeps the M4_C1
 *     pattern instead, so MIDI out never exceeds 31250 baud on any device.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef PLOYTEC_MIDI_H
#define PLOYTEC_MIDI_H

#include "ploytec_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MIDI out pace: 80% of 31250 baud at 10 bits per byte (3125 bytes/s) */
#define PLOYTEC_MIDI_OUT_BYTES_PER_SEC  2500

#define PLOYTEC_MIDI_IN_MAX_SLOTS       41     /* NS7 framing; the Xone and M4_C1 frames have 4 */

/* MIDI on its own endpoints (bulk in / isoc out devices) */
#define PLOYTEC_MIDI_XFER_MAX           42     /* bytes per transfer, largest framing (NS7) */
#define PLOYTEC_MIDI_XFER_INTERVAL_US   1000   /* one transfer per millisecond */
#define PLOYTEC_MIDI_XFER_PATTERN       8      /* entries in the M4_C1 byte-count pattern */
#define PLOYTEC_MIDI_XFER_CONTROL_IDLE  0xE0   /* last byte of a transfer (seen in captures) */

/* MIDI framing on its own endpoints, see above */
enum ploytec_midi_xfer_framing {
	PLOYTEC_MIDI_XFER_M4_C1,
	PLOYTEC_MIDI_XFER_NS7,
};
#define PLOYTEC_OUT_SUBPKTS             (PLOYTEC_FRAMES_PER_PKT / PLOYTEC_FRAMES_PER_OUT_SUBPKT)

/*
 * struct ploytec_midi - MIDI framing for one stream at one sample rate.
 * Set up with ploytec_midi_init() whenever the rate changes.
 */
struct ploytec_midi {
	uint32_t in_slots;   /* MIDI byte slots per MIDI IN transfer */
	uint32_t out_step;   /* added per sub-packet: bytes/s * frames per sub-packet */
	uint32_t out_rate;   /* sample rate: a slot opens each time the sum passes it */
	uint32_t out_acc;    /* pacing accumulator, always < out_rate */
	uint32_t xfer_idx;   /* MIDI out transfer pattern position (own endpoint) */
	uint32_t xfer_size;  /* bytes per MIDI IN / OUT transfer on own endpoints */
};

/*
 * ploytec_midi_init_common - The parts every framing shares.
 */
static inline void ploytec_midi_init_common(struct ploytec_midi *m, uint32_t rate, uint32_t in_slots)
{
	m->in_slots = in_slots;
	m->out_step = PLOYTEC_MIDI_OUT_BYTES_PER_SEC * PLOYTEC_FRAMES_PER_OUT_SUBPKT;
	m->out_rate = rate;
	m->out_acc = 0;
	m->xfer_idx = 0;
	m->xfer_size = 9;
}

/*
 * ploytec_midi_init_xfer - Set up MIDI framing for a device with its own MIDI
 * endpoints (bulk in / isoc out), in @framing.
 * @rate: sample rate in Hz
 */
static inline void ploytec_midi_init_xfer(struct ploytec_midi *m, uint32_t rate,
					  enum ploytec_midi_xfer_framing framing)
{
	uint32_t ns7 = framing == PLOYTEC_MIDI_XFER_NS7;

	ploytec_midi_init_common(m, rate, ns7 ? PLOYTEC_MIDI_XFER_MAX - 1 : 4);
	m->xfer_size = ns7 ? PLOYTEC_MIDI_XFER_MAX : 9;
}

/*
 * ploytec_midi_init - Set up MIDI framing for a Xone stream (slots by rate).
 * @rate:            sample rate in Hz
 * @pcm_in_int_1024: PCM IN endpoint is interrupt with wMaxPacketSize 1024
 */
static inline void ploytec_midi_init(struct ploytec_midi *m, uint32_t rate, bool pcm_in_int_1024)
{
	ploytec_midi_init_common(m, rate, rate <= 48000 ? 4 : (pcm_in_int_1024 ? 2 : 1));
}

/*
 * ploytec_midi_out_slots - Pace the next output packet.
 * Sets bit k of *@mask if sub-packet k may carry a MIDI byte and returns how
 * many may. Advances the pace whether or not there is data, so unused slots
 * don't add up to a burst later.
 */
static inline uint32_t ploytec_midi_out_slots(struct ploytec_midi *m, uint32_t *mask)
{
	uint32_t bits = 0, count = 0, k, open;

	for (k = 0; k < PLOYTEC_OUT_SUBPKTS; k++) {
		m->out_acc += m->out_step;
		open = m->out_acc >= m->out_rate;
		m->out_acc -= open * m->out_rate;
		bits |= open << k;
		count += open;
	}
	*mask = bits;
	return count;
}

/*
 * ploytec_midi_out_place - Write an output packet's MIDI slots.
 * @bytes holds one byte per bit set in @mask (PLOYTEC_MIDI_IDLE_BYTE where
 * there was no data); they go into those sub-packets in order. All other
 * slots get PLOYTEC_MIDI_IDLE_BYTE. The control bytes stay as
 * ploytec_out_init_packet() wrote them.
 */
static inline void ploytec_midi_out_place(enum ploytec_out_layout layout, uint8_t *pkt,
					  uint32_t mask, const uint8_t bytes[PLOYTEC_OUT_SUBPKTS])
{
	uint32_t k, i = 0, open;

	for (k = 0; k < PLOYTEC_OUT_SUBPKTS; k++) {
		open = (mask >> k) & 1;
		pkt[ploytec_out_midi_offset(layout, k)] = open ? bytes[i & (PLOYTEC_OUT_SUBPKTS - 1)]
							       : PLOYTEC_MIDI_IDLE_BYTE;
		i += open;
	}
}

/*
 * ploytec_midi_xfer_begin - Start the next MIDI out transfer on its own endpoint
 * (xfer_size bytes). Fills @buf with 0xFD and the control byte, and returns how
 * many MIDI bytes go at its start: 4 or 3 in every framing, one transfer per
 * millisecond is 3125 bytes/s (31250 baud). Bytes left over stay 0xFD.
 */
static inline uint32_t ploytec_midi_xfer_begin(struct ploytec_midi *m,
					       uint8_t buf[PLOYTEC_MIDI_XFER_MAX])
{
	uint32_t i;

	for (i = 0; i < m->xfer_size - 1; i++)
		buf[i] = PLOYTEC_MIDI_IDLE_BYTE;
	buf[m->xfer_size - 1] = PLOYTEC_MIDI_XFER_CONTROL_IDLE;

	/* _USBMidiPattern_USB1: 4,3,3,3,3,3,3,3 */
	return 3 + ((m->xfer_idx++ & (PLOYTEC_MIDI_XFER_PATTERN - 1)) == 0);
}

/*
 * ploytec_midi_in - The MIDI bytes of one MIDI IN transfer.
 * Copies the non-idle bytes among the first in_slots bytes of @frame (@len
 * received) to @out (room for PLOYTEC_MIDI_IN_MAX_SLOTS) and returns how
 * many. Drops the control byte.
 */
static inline uint32_t ploytec_midi_in(const struct ploytec_midi *m, const uint8_t *frame,
				       uint32_t len, uint8_t out[PLOYTEC_MIDI_IN_MAX_SLOTS])
{
	uint32_t n = len < m->in_slots ? len : m->in_slots;
	uint32_t count = 0, i;

	for (i = 0; i < n; i++) {
		out[count] = frame[i];
		count += frame[i] != PLOYTEC_MIDI_IDLE_BYTE;
	}
	return count;
}

#ifdef __cplusplus
}
#endif

#endif /* PLOYTEC_MIDI_H */
