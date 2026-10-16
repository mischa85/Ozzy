/* SPDX-License-Identifier: MIT */
/*
 * Ozzy Device Quirks: the quirks field of ozzy_devices.def rows
 *
 * Bits that tell a family's code what one device does differently, each from the
 * official driver's per-VID/PID special cases. Separate from ozzy_devices.h so
 * protocol code (the shared handshake) can use them without the device table.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_QUIRKS_H
#define OZZY_QUIRKS_H

enum ozzy_quirk {
	/*
	 * Ploytec own-endpoint MIDI in the Numark NS7 framing (42-byte transfers,
	 * ploytec_midi.h), the official driver's kInACR 8.
	 */
	OZZY_QUIRK_MIDI_NS7 = 1u << 0,

	/*
	 * No AJ input selector read-modify-write ('I' wIndex 0) in the handshake: the
	 * official updateAjInputSelector returns before any request for this device
	 * (Reloop Jockey 3 ME).
	 */
	OZZY_QUIRK_NO_AJ_SELECTOR = 1u << 1,

	/*
	 * ESI U-series (U168 XT, U86 XT, U108 PRE): ESI's input selector
	 * (updateAjInputSelector_Esu186) with the 'A' request in place of the Xone
	 * one, and the CPLD byte write ('I' wIndex 1, setEsuCpldByte), before the
	 * rate is set (ploytec_handshake.c).
	 */
	OZZY_QUIRK_ESI = 1u << 2,

	/*
	 * ESI U168 XT: the extra 'A' request (wValue 0x349) that configurationDone
	 * sends to 2573:0012 only.
	 */
	OZZY_QUIRK_ESI_U168 = 1u << 3,
};

#endif /* OZZY_QUIRKS_H */
