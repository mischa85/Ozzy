#include "PloytecCodec.h"
#include "PloytecProtocol.h"
#include "../../../common/devices/ploytec/ploytec_codec.h"
#include "../../Shared/OzzySharedData.h"
#include <cstring>
#include <cmath>
#include <utility>

// The bit-interleaving itself is the shared codec (common/devices/ploytec/ploytec_codec.c),
// which works on S24_3LE. The HAL works in float, so convert around it. The channel count and
// frame codec are template parameters: the per-channel conversion is unrolled at compile time
// and the codec call is direct, so the I/O path never branches on the channel count.
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

template <uint32_t Channels, ploytec_frame_fn Encode>
static inline void EncodeFrame(uint8_t* dst, const float* src) {
    uint8_t s24[Channels * 3];
    FloatToS24(s24, src, std::make_index_sequence<Channels>{});
    Encode(dst, s24);
}

template <uint32_t Channels, ploytec_frame_fn Decode>
static inline void DecodeFrame(float* dst, const uint8_t* src) {
    uint8_t s24[Channels * 3];
    Decode(s24, src);
    S24ToFloat(dst, s24, std::make_index_sequence<Channels>{});
}

// Ring buffer mirrors the USB packet structure for zero-copy: logical packets of 80 frames at
// kOzzyMaxPacketSize stride. Frame offsets inside a packet come from the shared layout helpers.
template <enum ploytec_out_layout Layout, uint32_t Channels, ploytec_frame_fn Encode>
static void WriteOutput(uint8_t* ringBuffer, const float* srcFrames, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize, uint32_t bytesPerFrame) {
    for (uint32_t i = 0; i < frameCount; i++) {
        uint32_t sampleOffset = (uint32_t)((sampleTime + i) % ringSize);
        uint32_t logicalPacket = sampleOffset / PLOYTEC_FRAMES_PER_PKT;
        uint32_t frameInLogicalPacket = sampleOffset % PLOYTEC_FRAMES_PER_PKT;

        uint32_t byteOffset = (logicalPacket * kOzzyMaxPacketSize) + ploytec_out_frame_offset(Layout, frameInLogicalPacket);
        EncodeFrame<Channels, Encode>(ringBuffer + byteOffset, srcFrames + (i * Channels));
    }
}

// Read input samples from ring buffer
// Ring buffer layout: logical packets at kOzzyMaxPacketSize stride, no MIDI in input
template <uint32_t Channels, ploytec_frame_fn Decode>
static void ReadInput(float* dstFrames, const uint8_t* ringBuffer, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize, uint32_t bytesPerFrame) {
    for (uint32_t i = 0; i < frameCount; i++) {
        uint32_t sampleOffset = (uint32_t)((sampleTime + i) % ringSize);

        // Input is simpler - no MIDI interleaving, just linear samples within logical packets
        uint32_t logicalPacket = sampleOffset / PLOYTEC_FRAMES_PER_PKT;
        uint32_t frameInLogicalPacket = sampleOffset % PLOYTEC_FRAMES_PER_PKT;

        uint32_t byteOffset = (logicalPacket * kOzzyMaxPacketSize) + (frameInLogicalPacket * PLOYTEC_IN_FRAME_SIZE);
        DecodeFrame<Channels, Decode>(dstFrames + (i * Channels), ringBuffer + byteOffset);
    }
}

WriteOutputFunc PloytecOutputWriter(bool bulkLayout, uint32_t channels) {
    switch (channels) {
    case 8: return bulkLayout ? WriteOutput<PLOYTEC_OUT_LAYOUT_BULK, 8, ploytec_encode_frame_8ch>
                              : WriteOutput<PLOYTEC_OUT_LAYOUT_INT, 8, ploytec_encode_frame_8ch>;
    case 2: return bulkLayout ? WriteOutput<PLOYTEC_OUT_LAYOUT_BULK, 2, ploytec_encode_frame_2ch>
                              : WriteOutput<PLOYTEC_OUT_LAYOUT_INT, 2, ploytec_encode_frame_2ch>;
    default: return nullptr;
    }
}

ReadInputFunc PloytecInputReader(uint32_t channels) {
    switch (channels) {
    case 8: return ReadInput<8, ploytec_decode_frame_8ch>;
    case 16: return ReadInput<16, ploytec_decode_frame_16ch>;
    default: return nullptr;
    }
}

// Clear output buffer: zero all PCM frames, leave the MIDI and sync bytes untouched
static void PloytecClearOutput(enum ploytec_out_layout layout, uint8_t* outputBuffer) {
    for (uint32_t logicalPacket = 0; logicalPacket < kOzzyNumPackets; logicalPacket++) {
        uint8_t* packet = outputBuffer + logicalPacket * kOzzyMaxPacketSize;
        for (uint32_t frame = 0; frame < PLOYTEC_FRAMES_PER_PKT; frame++) {
            memset(packet + ploytec_out_frame_offset(layout, frame), 0, PLOYTEC_OUT_FRAME_SIZE);
        }
    }
}

void PloytecClearOutputBulk(uint8_t* outputBuffer, uint32_t bufferSize) {
    PloytecClearOutput(PLOYTEC_OUT_LAYOUT_BULK, outputBuffer);
}

void PloytecClearOutputInterrupt(uint8_t* outputBuffer, uint32_t bufferSize) {
    PloytecClearOutput(PLOYTEC_OUT_LAYOUT_INT, outputBuffer);
}
