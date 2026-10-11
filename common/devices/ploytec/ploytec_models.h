/* SPDX-License-Identifier: MIT */
/*
 * Ploytec Device Models
 *
 * Per-model PCM topology and sample rates, shared by the Linux and macOS
 * drivers. Channel counts are the number of audio lanes a model uses in the
 * junction frame (up to PLOYTEC_MAX_IN_CHANNELS in, PLOYTEC_MAX_OUT_CHANNELS
 * out, see ploytec_codec.h). Rates come from the official drivers' per-device
 * format table (kConfigurationData, see .claude/re/ploytec_sample_rates.md)
 * where we have it.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_PLOYTEC_MODELS_H
#define OZZY_PLOYTEC_MODELS_H

#if defined(__KERNEL__) && defined(__linux__)
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#include "ploytec_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLOYTEC_MAX_RATES 4

struct ploytec_model {
	uint16_t pid;
	const char *name;
	uint8_t in_channels;     /* lanes used in the 64-byte input frame */
	uint8_t out_channels;    /* lanes used in the 48-byte output frame */
	uint8_t num_rates;
	uint32_t rates[PLOYTEC_MAX_RATES];   /* ascending */
};

/*
 * ploytec_model_for_pid - Topology and rates for a Ploytec product ID.
 * Unknown IDs get the Xone 8x8 defaults.
 */
static inline const struct ploytec_model *ploytec_model_for_pid(uint16_t pid)
{
	static const struct ploytec_model models[] = {
		/* XONE_DB4.kext 3.4.9 */
		{ PLOYTEC_PID_XONE_DB4, "Xone:DB4",  8, 8, 4, { 44100, 48000, 88200, 96000 } },
		/* XONE_DB2.kext 3.4.9: 48 kHz only */
		{ PLOYTEC_PID_XONE_DB2, "Xone:DB2",  8, 8, 1, { 48000 } },
		/* no official table yet: same as DB4 */
		{ PLOYTEC_PID_XONE_DX,  "Xone:DX",   8, 8, 4, { 44100, 48000, 88200, 96000 } },
		{ PLOYTEC_PID_XONE_4D,  "Xone:4D",   8, 8, 4, { 44100, 48000, 88200, 96000 } },
		/* experimental: 6 analog inputs on 8 lanes (#77), no official table */
		{ PLOYTEC_PID_XONE_2D,  "Xone:2D",   8, 8, 4, { 44100, 48000, 88200, 96000 } },
	};
	unsigned int i;

	for (i = 0; i < sizeof(models) / sizeof(models[0]); i++) {
		if (models[i].pid == pid)
			return &models[i];
	}
	return &models[0];
}

#ifdef __cplusplus
}
#endif

#endif /* OZZY_PLOYTEC_MODELS_H */
