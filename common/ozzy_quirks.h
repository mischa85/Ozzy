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
	 * Write the S/PDIF output registers ('A' 0x0D..0x11, writeDigitalOutMode) after the
	 * rate is set (plist flag PGDevice+0x7a6), in the mode of the S/PDIF out mode setting
	 * (ozzy_settings.h, default consumer). MPC Renaissance, every Tascam but the US-200.
	 */
	OZZY_QUIRK_SPDIF_OUT = 1u << 7,

	/*
	 * Write the digital output selector, 'I' wIndex 2 = 0, before the rate is set
	 * (writeDigitalOutSelector, plist flag PGDevice+0x7ac). Tascam US-1641, US-1800,
	 * US-2000. Despite the name not their digital output channels choice (that is output
	 * routing); Tascam's panel sets it only from its USB 1.1 mode code (device_settings.md).
	 */
	OZZY_QUIRK_DIGITAL_OUT_SEL = 1u << 8,

	/*
	 * The CPLD byte write ('I' wIndex 1 = 0xE7, setEsuCpldByte) before the rate is set:
	 * ESI U-series and ESI ESU1808 (0A92:0111).
	 */
	OZZY_QUIRK_ESI_CPLD = 1u << 9,

	/*
	 * ESI ESU1808: output lanes 9/10 carry the S/PDIF mirror of an output pair, 1/2 by
	 * default (official dmaEncode10_12/34/56/78, the DigitalOutSelector setting; Ozzy:
	 * ploytec_spdif_mirror and the S/PDIF mirror setting). Only with the bulk output
	 * layout: on interrupt (964-byte) output the official driver uses the plain 8-channel
	 * encoder.
	 */
	OZZY_QUIRK_SPDIF_MIRROR = 1u << 10,

	/*
	 * S/PDIF in that can clock the device: the Clock Source setting (ozzy_settings.h),
	 * bit 1 of 'I' wIndex 0, and following the rate the device locks to. Rows whose plist
	 * has digital in (no flag 0x7a0, which makes the official driver force lock off):
	 * MPC Renaissance, Lexicon IO42/IO82, Tascam US-144/MKII/600/1641/1800/2000. Not the
	 * ESI devices, whose selector has three states with an open mapping.
	 */
	OZZY_QUIRK_DIGITAL_IN = 1u << 11,

	/*
	 * Output routing (ozzy_route.h): which host playback pair each device output pair plays.
	 * The official pcmTo24LSB_US144_* encoders, on plist flag 0x7a9 devices: Tascam
	 * US-144MKII, US-600, US-200 (4 outputs each).
	 */
	OZZY_QUIRK_OUT_ROUTING = 1u << 12,

	/*
	 * Input routing (ozzy_route.h): which device input pair each host capture pair carries.
	 * The official input post-processors: Tascam US-144MKII (_postprocessUS144_*, 2 pairs)
	 * and US-600 (_postprocessUS600_*, 3 pairs).
	 */
	OZZY_QUIRK_IN_ROUTING = 1u << 13,

	/*
	 * Selectable PCM transfer sizes (OZZY_SETTING_PCM_OUT_FRAMES / _IN_FRAMES). Not in
	 * ozzy_devices.def: the engine family adds them (its setting_quirks), as they follow its
	 * transport: Ploytec bulk / interrupt out has both, the isochronous family only in.
	 */
	OZZY_QUIRK_PCM_OUT_FRAMES = 1u << 14,
	OZZY_QUIRK_PCM_IN_FRAMES = 1u << 15,

	/*
	 * Selectable PCM transfers in flight (OZZY_SETTING_PCM_OUT_QUEUE / _IN_QUEUE), added by the
	 * engine family like the sizes: Ploytec bulk / interrupt out has both, the isochronous family
	 * only in (with its own default, its engine's setting_default).
	 */
	OZZY_QUIRK_PCM_OUT_QUEUE = 1u << 16,
	OZZY_QUIRK_PCM_IN_QUEUE = 1u << 17,

	/*
	 * The clock anchors' spacing (OZZY_SETTING_IO_BUFFER_MAX), added by the engine family only
	 * where the platform reads the anchors (ozzy_engine_set_anchors: the macOS kext, the WinUSB
	 * harness); Linux and Windows report positions per transfer instead.
	 */
	OZZY_QUIRK_CLOCK_ANCHORS = 1u << 18,

	/*
	 * Wait 1000 ms before the setup requests (Musonik PHASE X64): the 2.0.4 kext's
	 * initDeviceSpecificData sets PGDevice+0x2d8 = 1000, slept in configurationDone.
	 * Windows 2.9.34 waits 600 ms, after the first configuration only.
	 */
	OZZY_QUIRK_WAIT_1000MS = 1u << 19,

	/*
	 * Write the WM8776 codec defaults in configurationDone (Musonik PHASE X64): Windows
	 * 2.9.34's WM8776SetDefault, which sets inputs 1/2 to Mic/Line, inputs 3/4 to Aux,
	 * playback on both output pairs and direct monitoring off. The Mac kext leaves the codecs
	 * to its control panel, so their power-on state is unknown.
	 */
	OZZY_QUIRK_WM8776 = 1u << 20,
};

#endif /* OZZY_QUIRKS_H */
