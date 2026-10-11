/* SPDX-License-Identifier: MIT */
/*
 * Ploytec Bit-Interleaved Audio Codec
 *
 * The Ploytec chipset uses a proprietary bit-interleaved PCM encoding.
 * Eight channels of 24-bit audio are spread across device frames using
 * a bit-scatter pattern rather than simple byte packing.
 *
 * These functions convert between standard S24_3LE interleaved audio
 * (3 bytes per sample) and the Ploytec device format (48 bytes per output
 * frame, 64 bytes per input frame). The frame format is the same for every
 * junction device; a device only differs in how many channel lanes carry
 * audio (up to 8 out, up to 16 in), like the official driver's dmaEncodeNN /
 * dmaDecode16ToNN functions (see .claude/re/ploytec_device_table.md).
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
 * @dest:     output buffer, must be at least 48 bytes (PLOYTEC_OUT_FRAME_SIZE)
 * @src:      input buffer, @channels x 3 bytes of S24_3LE interleaved audio
 * @channels: channels in @src, 1..PLOYTEC_MAX_OUT_CHANNELS; unused lanes are silent
 *
 * Scatters the 24-bit samples across 48 bytes using the Ploytec bit-interleaving
 * scheme: odd channels (1,3,5,7) in bits 0..3 of the first 24 bytes, even channels
 * (2,4,6,8) in bits 0..3 of the second 24 bytes, MSB first.
 */
void ploytec_encode_frame(uint8_t *dest, const uint8_t *src, unsigned int channels);

/*
 * ploytec_decode_frame - Convert one Ploytec device frame to S24_3LE.
 * @dest:     output buffer, @channels x 3 bytes
 * @src:      input buffer, 64 bytes of Ploytec capture data (PLOYTEC_IN_FRAME_SIZE)
 * @channels: channels to decode, 1..PLOYTEC_MAX_IN_CHANNELS
 *
 * Gathers bits from the 64-byte input frame: bytes 0x00..0x17 carry the odd
 * channels (1,3,..,15) in bits 0..7, bytes 0x20..0x37 the even channels
 * (2,4,..,16), one bit of each channel per byte, MSB first.
 */
void ploytec_decode_frame(uint8_t *dest, const uint8_t *src, unsigned int channels);

#ifdef __cplusplus
}
#endif

#endif /* OZZY_PLOYTEC_CODEC_H */
