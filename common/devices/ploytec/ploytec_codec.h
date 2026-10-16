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
 * audio (up to 10 out, up to 16 in), like the official driver's dmaEncodeNN /
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
 * Frame codecs, one unrolled function per channel count (like the official
 * driver's dmaEncodeNN / dmaDecode16ToNN). There is no branching per frame:
 * look the function up once with ploytec_frame_encoder()/_decoder() when the
 * model is known, then call it for every frame.
 *
 * Encoders: @src is channels x 3 bytes S24_3LE, @dest a 48-byte output frame
 * (PLOYTEC_OUT_FRAME_SIZE). Odd channels go to bits 0..4 of bytes 0x00..0x17,
 * even channels to bits 0..4 of bytes 0x18..0x2F, MSB first; unused lanes are silent.
 *
 * Decoders: @src is a 64-byte input frame (PLOYTEC_IN_FRAME_SIZE), @dest gets
 * channels x 3 bytes S24_3LE. Odd channels come from bits 0..7 of bytes
 * 0x00..0x17, even channels from bytes 0x20..0x37, MSB first.
 */
typedef void (*ploytec_frame_fn)(uint8_t *dest, const uint8_t *src);

void ploytec_encode_frame_10ch(uint8_t *dest, const uint8_t *src);
void ploytec_encode_frame_8ch(uint8_t *dest, const uint8_t *src);
void ploytec_encode_frame_6ch(uint8_t *dest, const uint8_t *src);
void ploytec_encode_frame_4ch(uint8_t *dest, const uint8_t *src);
void ploytec_encode_frame_2ch(uint8_t *dest, const uint8_t *src);
void ploytec_decode_frame_2ch(uint8_t *dest, const uint8_t *src);
void ploytec_decode_frame_4ch(uint8_t *dest, const uint8_t *src);
void ploytec_decode_frame_6ch(uint8_t *dest, const uint8_t *src);
void ploytec_decode_frame_8ch(uint8_t *dest, const uint8_t *src);
void ploytec_decode_frame_10ch(uint8_t *dest, const uint8_t *src);
void ploytec_decode_frame_16ch(uint8_t *dest, const uint8_t *src);

/*
 * The ESU1808's S/PDIF mirror of output pair 1/2 (lanes 9/10), added to a frame from
 * ploytec_encode_frame_8ch; @prev_even is the previous encoded frame's bytes 0x18..0x2F.
 */
void ploytec_spdif_mirror_12(uint8_t *dest, const uint8_t *prev_even);

/* The codec for a channel count, or NULL if there is none. Call once, not per frame. */
ploytec_frame_fn ploytec_frame_encoder(unsigned int channels);
ploytec_frame_fn ploytec_frame_decoder(unsigned int channels);

#ifdef __cplusplus
}
#endif

#endif /* OZZY_PLOYTEC_CODEC_H */
