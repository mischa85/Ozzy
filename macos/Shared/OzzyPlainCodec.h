/* SPDX-License-Identifier: MIT */
/*
 * Plain PCM for the HAL: float <-> S24_3LE samples, and the output writer for devices
 * whose PCM out takes interleaved S24_3LE frames as they are (kOzzyDeviceFlagPlainOutput).
 * Their outputBuffer is a ring of frames that the kext's isochronous transfers read
 * directly, so writing a frame is just converting its samples into place.
 *
 * Header-only, user space (the HAL). Channel counts are template parameters, so the
 * I/O path doesn't branch on them.
 */
#ifndef OzzyPlainCodec_h
#define OzzyPlainCodec_h

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <utility>

// One sample, without branches: clamp with fminf/fmaxf (min/max instructions), sign-extend
// with a shift pair.
static inline void StoreS24(uint8_t* dst, float sample) {
    int32_t s = (int32_t)(fmaxf(-1.0f, fminf(sample, 8388607.0f / 8388608.0f)) * 8388608.0f);
    dst[0] = s & 0xFF;
    dst[1] = (s >> 8) & 0xFF;
    dst[2] = (s >> 16) & 0xFF;
}

static inline float LoadS24(const uint8_t* src) {
    int32_t s = (int32_t)((uint32_t)(src[0] | (src[1] << 8) | (src[2] << 16)) << 8) >> 8;
    return (float)s / 8388608.0f;
}

// Per-channel conversion, unrolled at compile time by a fold over the channel indices
template <size_t... I>
static inline void FloatToS24(uint8_t* s24, const float* src, std::index_sequence<I...>) {
    (StoreS24(s24 + I * 3, src[I]), ...);
}

template <size_t... I>
static inline void S24ToFloat(float* dst, const uint8_t* s24, std::index_sequence<I...>) {
    ((dst[I] = LoadS24(s24 + I * 3)), ...);
}

// Frame ring of @ringSize frames: frame f lives at f * Channels * 3
template <uint32_t Channels>
static void PlainWriteOutput(uint8_t* ringBuffer, const float* srcFrames, uint64_t sampleTime,
                             uint32_t frameCount, uint32_t ringSize, uint32_t bytesPerFrame) {
    uint32_t f = (uint32_t)(sampleTime % ringSize);
    for (uint32_t i = 0; i < frameCount; i++) {
        FloatToS24(ringBuffer + f * (Channels * 3), srcFrames + i * Channels, std::make_index_sequence<Channels>{});
        f++;
        f -= (f == ringSize) * ringSize;
    }
}

typedef void (*PlainWriteOutputFunc)(uint8_t*, const float*, uint64_t, uint32_t, uint32_t, uint32_t);

// Writer for a channel count, picked once per device (nullptr if there is none)
static inline PlainWriteOutputFunc PlainOutputWriter(uint32_t channels) {
    switch (channels) {
    case 2: return PlainWriteOutput<2>;
    case 4: return PlainWriteOutput<4>;
    case 6: return PlainWriteOutput<6>;
    case 8: return PlainWriteOutput<8>;
    default: return nullptr;
    }
}

// Silence: plain frames have no framing bytes to keep
static inline void PlainClearOutput(uint8_t* outputBuffer, uint32_t bufferSize) {
    memset(outputBuffer, 0, bufferSize);
}

#endif
