/* SPDX-License-Identifier: MIT */
/*
 * MIDI-only devices that speak the USB MIDI class protocol but need Ozzy because macOS's class
 * driver won't take them. The Xone:1D's bulk endpoints have wMaxPacketSize 4, which USB doesn't
 * allow for full speed bulk; Linux's snd-usb-audio accepts it, macOS doesn't. MIDI goes through
 * the same shared memory rings as the Ploytec devices, so OzzyMIDI handles both. No audio: the
 * engine publishes 0 channels and the HAL skips the device.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef USB_MIDI_ENGINE_H
#define USB_MIDI_ENGINE_H

#include "../../core/ozzy_engine.h"
#include "../../../common/usb_midi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USB_MIDI_ENGINE_READS 2

/*
 * struct usb_midi_engine
 * @encoder:  only touched while @out_busy is held
 * @out_busy: one OUT transfer in flight; hands the sender role between the MIDI client's kick
 *            and the OUT completion
 */
struct usb_midi_engine {
	struct ozzy_engine base;
	struct usb_midi_encoder encoder;
	bool running;
	bool out_busy;
	uint32_t read_errors[USB_MIDI_ENGINE_READS];
};

void usb_midi_engine_init(struct usb_midi_engine *me, const struct ozzy_device *model);

#ifdef __cplusplus
}
#endif

#endif /* USB_MIDI_ENGINE_H */
