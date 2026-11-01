/* SPDX-License-Identifier: MIT */
/*
 * Ploytec C-port: the serial control bus of the ESI U168 XT's converters, carried in the PCM
 * output stream
 *
 * The control byte after each MIDI out slot (ploytec_midi.h) drives a port of GPIO lines in the
 * device. ESI's control panel (U168 XT Panel 1.4) bit-bangs two converters on it, one control
 * byte per output sub-packet; the kext only queues the bytes (dispatch 0x24, AJ::sendCPort) and
 * puts each in a control byte as byte | 0x18, else 0xFF (PGDevice::rtsProcessBulkOut).
 *
 *   bit 7     chip select of both converters, low during a write
 *   bits 6/5  data / clock of the DAC (an AK4358: its attenuator registers)
 *   bits 1/0  data / clock of the S/PDIF transmitter (an AK4114: its channel status registers)
 *   bits 4..2 high
 *
 * A write: 0xFF, 0x7F (select), 16 bits MSB first as AKM's 3-wire format has them (2-bit chip
 * address, R/W = 1, 5-bit register, 8-bit data), each bit as clock low then clock high, then
 * 0xFF. The panel's chip addresses: 2 for the DAC (bits 1 0 1), 0 for the transmitter (0 0 1).
 * .claude/re/esi_u168xt_cport.md.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef PLOYTEC_CPORT_H
#define PLOYTEC_CPORT_H

#include "ploytec_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

enum ploytec_cport_chip {
	PLOYTEC_CPORT_DAC,             /* AK4358: chip address 2, clock bit 5, data bit 6 */
	PLOYTEC_CPORT_DIT,             /* AK4114: chip address 0, clock bit 0, data bit 1 */
};

enum ploytec_cport_byte {
	PLOYTEC_CPORT_IDLE        = 0xFF,   /* also the control byte with nothing queued */
	PLOYTEC_CPORT_SELECT      = 0x7F,
	PLOYTEC_CPORT_FORCED      = 0x18,   /* rtsProcessBulkOut ORs these into every byte it sends */
	PLOYTEC_CPORT_WRITE_BYTES = 2 + 16 * 2 + 1,
};

/* AK4358 attenuators of outputs 1-8 (the panel's table: 0x0A is a control register) and
 * their levels: 0xFF is 0 dB, a step 0.5 dB */
static const uint8_t ploytec_cport_dac_att_regs[8] = { 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0B, 0x0C };

enum ploytec_cport_reg {
	PLOYTEC_CPORT_ATT_0DB     = 0xFF,
	PLOYTEC_CPORT_ATT_MINUS10 = 0xFF - 10,   /* the panel's "-10": 10 steps (5 dB) down */
	PLOYTEC_CPORT_DIT_TX_CS0  = 0x0D,        /* AK4114 transmit channel status byte 0; bit 0 is
						    IEC 60958's professional flag */
};

/*
 * ploytec_cport_write - The bytes of one register write to @chip (PLOYTEC_CPORT_WRITE_BYTES of
 * them at @out), as the panel sends them. Returns how many.
 */
static inline uint32_t ploytec_cport_write(uint8_t *out, enum ploytec_cport_chip chip, uint8_t reg,
					   uint8_t value)
{
	/* Chip address, R/W, register, data: 16 bits */
	uint32_t word = ((chip == PLOYTEC_CPORT_DAC ? 2u : 0u) << 14) | (1u << 13) |
			((uint32_t)(reg & 0x1F) << 8) | value;
	uint32_t n = 0;
	int b;

	out[n++] = PLOYTEC_CPORT_IDLE;
	out[n++] = PLOYTEC_CPORT_SELECT;
	for (b = 15; b >= 0; b--) {
		bool one = (word >> b) & 1;

		if (chip == PLOYTEC_CPORT_DAC) {
			out[n++] = one ? 0x5F : 0x1F;   /* data bit 6, clock (bit 5) low */
			out[n++] = one ? 0x7F : 0x3F;   /* clock high */
		} else {
			out[n++] = one ? 0x7E : 0x7C;   /* data bit 1, clock (bit 0) low */
			out[n++] = one ? 0x7F : 0x7D;
		}
	}
	out[n++] = PLOYTEC_CPORT_IDLE;
	return n;
}

#ifdef __cplusplus
}
#endif

#endif /* PLOYTEC_CPORT_H */
