/* SPDX-License-Identifier: MIT */
/*
 * Ploytec junction devices with bulk or interrupt PCM out (the Xones and their relatives).
 * PCM and MIDI out share every output packet; PCM in and MIDI in have their own endpoints.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef PLOYTEC_ENGINE_H
#define PLOYTEC_ENGINE_H

#include "../../core/ozzy_engine.h"
#include "../../../common/devices/ploytec/ploytec_protocol.h"
#include "../../../common/devices/ploytec/ploytec_midi.h"
#include "../../../common/devices/ploytec/ploytec_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * struct ploytec_engine
 * @hw_sample_time: frames of completed PCM out packets (the device's clock)
 * @sample_rate:    what the device runs at (the S/PDIF lock rate while locked)
 * @default_rate:   the highest rate the device lists (what start asks for without a request)
 * @running:        completions resubmit (cleared by stop and suspend)
 * @pcm_in_int1024: PCM IN is interrupt, wMaxPacketSize 1024 (ploytec_midi.h)
 * @encode/@decode: frame codecs for the model's channel counts
 * @mirror:         ESU1808 S/PDIF mirror (bulk layout, 8 out)
 */
struct ploytec_engine {
	struct ozzy_engine base;
	uint64_t hw_sample_time;
	enum ploytec_out_layout out_layout;
	uint32_t sample_rate;
	uint32_t default_rate;
	volatile bool running;
	uint32_t packet_size_out;
	bool pcm_in_int1024;
	struct ploytec_midi midi;
	ploytec_frame_fn encode;
	ploytec_frame_fn decode;
	bool mirror;
};

void ploytec_engine_init(struct ploytec_engine *pe, const struct ozzy_device *model);

#ifdef __cplusplus
}
#endif

#endif /* PLOYTEC_ENGINE_H */
