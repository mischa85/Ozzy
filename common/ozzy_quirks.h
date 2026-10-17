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
	 * No AJ input selector step ('I' wIndex 0 read-modify-write, see AJ_BIT0) in the
	 * handshake: the official updateAjInputSelector returns before any request for
	 * this device (Reloop Jockey 3 ME). The status confirm (setAJDMAInputChannels)
	 * still goes out, as for every device.
	 */
	OZZY_QUIRK_NO_AJ_SELECTOR = 1u << 1,

	/*
	 * ESI U-series (U168 XT, U86 XT, U108 PRE): ESI's input selector
	 * (updateAjInputSelector_Esu186) with the 'A' request in place of the generic
	 * one, before the rate is set (ploytec_handshake.c).
	 */
	OZZY_QUIRK_ESI = 1u << 2,

	/*
	 * ESI U168 XT: the extra 'A' request (wValue 0x349) that configurationDone
	 * sends to 2573:0012 only.
	 */
	OZZY_QUIRK_ESI_U168 = 1u << 3,

	/*
	 * Wait 200 ms before the setup requests: the plist's kMsWait 200 (Numark NS7, NS6,
	 * MixDeck), which the official configurationDone sleeps (PGDevice+0x18) before
	 * any of its requests. The handshake sleeps after the firmware read.
	 */
	OZZY_QUIRK_WAIT_200MS = 1u << 4,

	/*
	 * Wait 100 ms before the setup requests (Tascam US-200): initDeviceSpecificData
	 * sets PGDevice+0x18 = 100 for it. Its plist's kMsWait 100, like the other
	 * Tascams', is cleared by initDeviceSpecificData before configurationDone runs.
	 */
	OZZY_QUIRK_WAIT_100MS = 1u << 5,

	/*
	 * The generic selector step also clears bit 0 of 'I' wIndex 0 (plist flag
	 * PGDevice+0x7a8; the driver's AjExtData[2] setting, default 0, possibly a
	 * USB 1.1 mode switch: setUSB1Mode). MPC Renaissance, Lexicon IO42/IO82, every
	 * Tascam but the US-200.
	 */
	OZZY_QUIRK_AJ_BIT0 = 1u << 6,

	/*
	 * Write the S/PDIF output registers ('A' 0x0D..0x11, writeDigitalOutMode, mode 0)
	 * after the rate is set (plist flag PGDevice+0x7a6). MPC Renaissance, every
	 * Tascam but the US-200.
	 */
	OZZY_QUIRK_SPDIF_OUT = 1u << 7,

	/*
	 * Write the digital output selector, 'I' wIndex 2 = 0, before the rate is set
	 * (writeDigitalOutSelector, plist flag PGDevice+0x7ac). Tascam US-1641, US-1800,
	 * US-2000.
	 */
	OZZY_QUIRK_DIGITAL_OUT_SEL = 1u << 8,

	/*
	 * The CPLD byte write ('I' wIndex 1 = 0xE7, setEsuCpldByte) before the rate is set:
	 * ESI U-series and ESI ESU1808 (0A92:0111).
	 */
	OZZY_QUIRK_ESI_CPLD = 1u << 9,

	/*
	 * ESI ESU1808: output lanes 9/10 carry the S/PDIF mirror of output pair 1/2 (official
	 * dmaEncode10_12 at its default selection; ploytec_spdif_mirror_12). Only with the
	 * bulk output layout: on interrupt (964-byte) output the official driver uses the
	 * plain 8-channel encoder.
	 */
	OZZY_QUIRK_SPDIF_MIRROR_12 = 1u << 10,

	/*
	 * S/PDIF in that can clock the device: the Clock Source setting (ozzy_settings.h),
	 * bit 1 of 'I' wIndex 0, and following the rate the device locks to. Rows whose plist
	 * has digital in (no flag 0x7a0, which makes the official driver force lock off):
	 * MPC Renaissance, Lexicon IO42/IO82, Tascam US-144/MKII/600/1641/1800/2000. Not the
	 * ESI devices, whose selector has three states with an open mapping.
	 */
	OZZY_QUIRK_DIGITAL_IN = 1u << 11,
};

#endif /* OZZY_QUIRKS_H */
