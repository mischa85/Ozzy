/* SPDX-License-Identifier: MIT */
/*
 * Ploytec isochronous in and out: the Tascam US-366 (.claude/re/tascam_us366.md, #91). PCM is
 * plain S24_3LE both ways at alternate setting 2 of interfaces 1 and 2, a fixed number of frames
 * each microframe and no feedback endpoint in use: ozzy_isoc_output paces out at the nominal
 * rate, ozzy_isoc_input takes what comes in. MIDI is USB-MIDI event packets on interface 3 (bulk
 * in, interrupt out). The handshake is the Windows driver's (ploytec_us3xx_handshake).
 * Written from a capture; untested on hardware.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef PLOYTEC_US3XX_ENGINE_H
#define PLOYTEC_US3XX_ENGINE_H

#include "../../core/ozzy_engine.h"
#include "../../core/ozzy_isoc_output.h"
#include "../../core/ozzy_isoc_input.h"
#include "../../../common/usb_midi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLOYTEC_US3XX_MIDI_READS  2   /* MIDI IN reads in flight, each half the MIDI IN memory */

/*
 * struct ploytec_us3xx_engine
 * @default_rate:  what start asks for without a request: 48 kHz, as the Windows driver started
 * @*_addr etc.:   the endpoints as the platform found them
 * @mixer_sent:    the 'M' / 'B' blocks went out (the first start after attach, as on Windows)
 * @midi_out_busy: one MIDI OUT transfer in flight; hands the sender role between the MIDI
 *                 client's kick and the OUT completion (@encoder is only touched while held)
 */
struct ploytec_us3xx_engine {
	struct ozzy_engine base;
	uint32_t sample_rate;
	uint32_t default_rate;
	volatile bool running;
	bool streamed;
	bool mixer_sent;

	uint8_t out_addr, in_addr, midi_in_addr, midi_out_addr;
	uint16_t out_max_packet, in_max_packet, midi_out_max_packet;
	uint8_t out_interval, in_interval;

	struct ozzy_isoc_output output;
	struct ozzy_isoc_input input;

	struct usb_midi_encoder encoder;
	bool midi_out_busy;
	uint32_t midi_read_errors[PLOYTEC_US3XX_MIDI_READS];
};

void ploytec_us3xx_engine_init(struct ploytec_us3xx_engine *ue, const struct ozzy_device *model);

#ifdef __cplusplus
}
#endif

#endif /* PLOYTEC_US3XX_ENGINE_H */
