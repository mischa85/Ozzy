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
 * audio (up to 16 out, up to 16 in), like the official driver's dmaEncodeNN /
 * dmaDecode16ToNN functions (see .claude/re/ploytec_device_table.md).
 *
 * Pure C with no platform dependencies -- usable in both kernel space
 * and userspace on any platform.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_PLOYTEC_CODEC_H
#define OZZY_PLOYTEC_CODEC_H

#if defined(__KERNEL__) && defined(__linux__)
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
 * (PLOYTEC_OUT_FRAME_SIZE). Odd channels go to bits 0..7 of bytes 0x00..0x17,
 * even channels to bits 0..7 of bytes 0x18..0x2F (channels 2k+1 and 2k+2 in bit k), MSB first; unused lanes are silent.
 *
 * Decoders: @src is a 64-byte input frame (PLOYTEC_IN_FRAME_SIZE), @dest gets
 * channels x 3 bytes S24_3LE. Odd channels come from bits 0..7 of bytes
 * 0x00..0x17, even channels from bytes 0x20..0x37, MSB first.
 */
typedef void (*ploytec_frame_fn)(uint8_t *dest, const uint8_t *src);

void ploytec_encode_frame_16ch(uint8_t *dest, const uint8_t *src);
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
 * The ESU1808's S/PDIF mirror of output pair @pair (0..3; lanes 9/10), added to a frame from
 * ploytec_encode_frame_8ch; @prev_even is the previous encoded frame's bytes 0x18..0x2F.
 */
void ploytec_spdif_mirror(uint8_t *dest, const uint8_t *prev_even, unsigned int pair);

/* The codec for a channel count, or NULL if there is none. Call once, not per frame. */
ploytec_frame_fn ploytec_frame_encoder(unsigned int channels);
ploytec_frame_fn ploytec_frame_decoder(unsigned int channels);

/*
 * Float codecs, user space only (the Linux and Windows drivers stay integer: they hand
 * S24_3LE to the OS and leave float to its audio engine). PLOYTEC_CODEC_F32 says whether
 * this build has them.
 *
 * Same shape as the S24_3LE codecs: @src / @dest is channels x float, full scale +-1.0.
 * Encoding clamps to the 24-bit range.
 */
#if !defined(__KERNEL__) && !defined(KERNEL) && !defined(_KERNEL_MODE)
#define PLOYTEC_CODEC_F32 1

#include <math.h>

/* One float to S24_3LE, clamped; without branches (fminf/fmaxf, then shifts) */
static inline void ploytec_f32_to_s24(uint8_t *dest, float sample)
{
	int32_t s = (int32_t)(fmaxf(-1.0f, fminf(sample, 8388607.0f / 8388608.0f)) * 8388608.0f);

	dest[0] = s & 0xFF;
	dest[1] = (s >> 8) & 0xFF;
	dest[2] = (s >> 16) & 0xFF;
}

/* One S24_3LE sample to float (sign-extended with a shift pair) */
static inline float ploytec_s24_to_f32(const uint8_t *src)
{
	int32_t s = (int32_t)((uint32_t)(src[0] | (src[1] << 8) | (src[2] << 16)) << 8) >> 8;

	return (float)s / 8388608.0f;
}

typedef void (*ploytec_frame_f32_enc_fn)(uint8_t *dest, const float *src);
typedef void (*ploytec_frame_f32_dec_fn)(float *dest, const uint8_t *src);

/*
 * Plain frames: channels x float <-> channels x 3 bytes S24_3LE, nothing interleaved. For the
 * devices whose PCM is plain S24_3LE (kOzzyDeviceFlagPlainOutput), and the first step of the
 * float frame codecs. Same types as the float frame codecs.
 */
void ploytec_f32_to_s24_16ch(uint8_t *dest, const float *src);
void ploytec_f32_to_s24_10ch(uint8_t *dest, const float *src);
void ploytec_f32_to_s24_8ch(uint8_t *dest, const float *src);
void ploytec_f32_to_s24_6ch(uint8_t *dest, const float *src);
void ploytec_f32_to_s24_4ch(uint8_t *dest, const float *src);
void ploytec_f32_to_s24_2ch(uint8_t *dest, const float *src);
void ploytec_s24_to_f32_2ch(float *dest, const uint8_t *src);
void ploytec_s24_to_f32_4ch(float *dest, const uint8_t *src);
void ploytec_s24_to_f32_6ch(float *dest, const uint8_t *src);
void ploytec_s24_to_f32_8ch(float *dest, const uint8_t *src);
void ploytec_s24_to_f32_10ch(float *dest, const uint8_t *src);
void ploytec_s24_to_f32_16ch(float *dest, const uint8_t *src);

/* Bit-interleaved float frames */

void ploytec_encode_frame_16ch_f32(uint8_t *dest, const float *src);
void ploytec_encode_frame_10ch_f32(uint8_t *dest, const float *src);
void ploytec_encode_frame_8ch_f32(uint8_t *dest, const float *src);
void ploytec_encode_frame_6ch_f32(uint8_t *dest, const float *src);
void ploytec_encode_frame_4ch_f32(uint8_t *dest, const float *src);
void ploytec_encode_frame_2ch_f32(uint8_t *dest, const float *src);
void ploytec_decode_frame_2ch_f32(float *dest, const uint8_t *src);
void ploytec_decode_frame_4ch_f32(float *dest, const uint8_t *src);
void ploytec_decode_frame_6ch_f32(float *dest, const uint8_t *src);
void ploytec_decode_frame_8ch_f32(float *dest, const uint8_t *src);
void ploytec_decode_frame_10ch_f32(float *dest, const uint8_t *src);
void ploytec_decode_frame_16ch_f32(float *dest, const uint8_t *src);

/* The float codec for a channel count, or NULL if there is none. Call once, not per frame. */
ploytec_frame_f32_enc_fn ploytec_frame_encoder_f32(unsigned int channels);
ploytec_frame_f32_dec_fn ploytec_frame_decoder_f32(unsigned int channels);
#endif

#ifdef __cplusplus
}
#endif

#endif /* OZZY_PLOYTEC_CODEC_H */
