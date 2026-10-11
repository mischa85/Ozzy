/* SPDX-License-Identifier: MIT */
/*
 * Ploytec Bit-Interleaved Audio Codec
 *
 * The Ploytec chipset uses a proprietary bit-interleaved PCM encoding.
 * Eight channels of 24-bit audio are spread across device frames using
 * a bit-scatter pattern rather than simple byte packing.
 *
 * These functions convert between standard S24_3LE interleaved audio
 * (3 bytes per sample, 24 bytes per 8-channel frame) and the Ploytec
 * device format (48 bytes per output frame, 64 bytes per input frame).
 *
 * Pure C with no platform dependencies -- usable in both kernel space
 * and userspace on any platform.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_PLOYTEC_CODEC_H
#define OZZY_PLOYTEC_CODEC_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ploytec_encode_frame - Convert one S24_3LE frame to Ploytec device format.
 * @dest: output buffer, must be at least 48 bytes (PLOYTEC_OUT_FRAME_SIZE)
 * @src:  input buffer, 24 bytes of S24_3LE interleaved audio (8 channels x 3 bytes)
 *
 * Scatters 8 channels of 24-bit samples across 48 bytes using the Ploytec
 * bit-interleaving scheme. Odd channels (1,3,5,7) occupy the first 24 output
 * bytes, even channels (2,4,6,8) occupy the second 24 bytes.
 */
void ploytec_encode_frame(uint8_t *dest, const uint8_t *src);

/*
 * ploytec_decode_frame - Convert one Ploytec device frame to S24_3LE.
 * @dest: output buffer, must be at least 24 bytes (8 channels x 3 bytes)
 * @src:  input buffer, 64 bytes of Ploytec capture data (PLOYTEC_IN_FRAME_SIZE)
 *
 * Gathers bits from the 64-byte Ploytec input frame and reassembles them
 * into 8 channels of 24-bit S24_3LE samples. The input frame uses a
 * bit-per-byte layout where each byte carries one bit per channel.
 */
void ploytec_decode_frame(uint8_t *dest, const uint8_t *src);

#ifdef __cplusplus
}
#endif

#endif /* OZZY_PLOYTEC_CODEC_H */
