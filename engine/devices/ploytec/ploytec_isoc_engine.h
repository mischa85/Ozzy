/* SPDX-License-Identifier: MIT */
/*
 * Ploytec bulk in / isochronous out devices (Akai EIE Pro, .claude/re/akai_eie_pro.md). The
 * handshake, PCM in and MIDI in are the Xone ones (ploytec_io.h). PCM out is plain S24_3LE,
 * run by ozzy_isoc_output from the device's feedback. MIDI out goes to its own endpoint in
 * 9-byte transfers, one per millisecond from the bus timer, like Linux. Endpoints are found
 * by type, as described in ploytec_defs.h for both platforms.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef PLOYTEC_ISOC_ENGINE_H
#define PLOYTEC_ISOC_ENGINE_H

#include "../../core/ozzy_engine.h"
#include "../../core/ozzy_isoc_output.h"
#include "../../../common/devices/ploytec/ploytec_protocol.h"
#include "../../../common/devices/ploytec/ploytec_midi.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * struct ploytec_isoc_engine
 * @sample_rate:   what the device runs at (the S/PDIF lock rate while locked)
 * @default_rate:  the highest rate (what start asks for without a request)
 * @*_addr etc.:   the endpoints as the platform found them
 * @midi_out_busy: one MIDI OUT transfer in flight
 */
struct ploytec_isoc_engine {
	struct ozzy_engine base;
	uint32_t sample_rate;
	uint32_t default_rate;
	struct ploytec_midi midi;
	volatile bool running;

	uint8_t out_addr, in_addr, fb_addr, midi_in_addr, midi_out_addr;
	uint16_t out_max_packet, fb_max_packet;
	uint8_t out_interval, fb_interval;

	struct ozzy_isoc_output output;
	bool midi_out_busy;
};

void ploytec_isoc_engine_init(struct ploytec_isoc_engine *pe, const struct ozzy_device *model);

#ifdef __cplusplus
}
#endif

#endif /* PLOYTEC_ISOC_ENGINE_H */
