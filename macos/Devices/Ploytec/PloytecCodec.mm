#include "PloytecCodec.h"
#include "PloytecProtocol.h"
#include "../../../common/devices/ploytec/ploytec_codec.h"
#include "../../Shared/OzzySharedData.h"
#include "../../Shared/OzzyPlainCodec.h"
#include <cstring>
#include <cmath>
#include <utility>

// The bit-interleaving itself is the shared codec (common/devices/ploytec/ploytec_codec.c),
// which works on S24_3LE. The HAL works in float, so convert around it. The channel count and
// frame codec are template parameters: the per-channel conversion is unrolled at compile time
// and the codec call is direct, so the I/O path never branches on the channel count. The
// sample conversion is shared with the plain codec (Shared/OzzyPlainCodec.h).
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

// The ESU1808 (bulk layout, 8 channels): the 8-channel frame plus the S/PDIF mirror of pair Pair,
// whose lane 9 is the pair's second channel of the frame before. That frame is in the ring
// already (written just before, or by the previous I/O cycle), so the mirror takes it from there.
template <unsigned int Pair>
static void WriteOutputSpdif(uint8_t* ringBuffer, const float* srcFrames, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize, uint32_t bytesPerFrame) {
    for (uint32_t i = 0; i < frameCount; i++) {
        uint32_t sampleOffset = (uint32_t)((sampleTime + i) % ringSize);
        uint32_t prevOffset = (sampleOffset ? sampleOffset : ringSize) - 1;
        uint8_t* dst = ringBuffer + (sampleOffset / PLOYTEC_FRAMES_PER_PKT) * kOzzyMaxPacketSize
                     + ploytec_out_frame_offset(PLOYTEC_OUT_LAYOUT_BULK, sampleOffset % PLOYTEC_FRAMES_PER_PKT);
        const uint8_t* prev = ringBuffer + (prevOffset / PLOYTEC_FRAMES_PER_PKT) * kOzzyMaxPacketSize
                            + ploytec_out_frame_offset(PLOYTEC_OUT_LAYOUT_BULK, prevOffset % PLOYTEC_FRAMES_PER_PKT);
        EncodeFrame<8, ploytec_encode_frame_8ch>(dst, srcFrames + (i * 8));
        ploytec_spdif_mirror(dst, prev + PLOYTEC_OUT_FRAME_SIZE / 2, Pair);
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

WriteOutputFunc PloytecOutputWriter(bool bulkLayout, uint32_t channels, bool spdifMirror, unsigned int mirrorPair) {
    if (spdifMirror && bulkLayout && channels == 8) {
        switch (mirrorPair) {
        case 1: return WriteOutputSpdif<1>;
        case 2: return WriteOutputSpdif<2>;
        case 3: return WriteOutputSpdif<3>;
        default: return WriteOutputSpdif<0>;
        }
    }
    switch (channels) {
    case 16: return bulkLayout ? WriteOutput<PLOYTEC_OUT_LAYOUT_BULK, 16, ploytec_encode_frame_16ch>
                               : WriteOutput<PLOYTEC_OUT_LAYOUT_INT, 16, ploytec_encode_frame_16ch>;
    case 10: return bulkLayout ? WriteOutput<PLOYTEC_OUT_LAYOUT_BULK, 10, ploytec_encode_frame_10ch>
                               : WriteOutput<PLOYTEC_OUT_LAYOUT_INT, 10, ploytec_encode_frame_10ch>;
    case 8: return bulkLayout ? WriteOutput<PLOYTEC_OUT_LAYOUT_BULK, 8, ploytec_encode_frame_8ch>
                              : WriteOutput<PLOYTEC_OUT_LAYOUT_INT, 8, ploytec_encode_frame_8ch>;
    case 6: return bulkLayout ? WriteOutput<PLOYTEC_OUT_LAYOUT_BULK, 6, ploytec_encode_frame_6ch>
                              : WriteOutput<PLOYTEC_OUT_LAYOUT_INT, 6, ploytec_encode_frame_6ch>;
    case 4: return bulkLayout ? WriteOutput<PLOYTEC_OUT_LAYOUT_BULK, 4, ploytec_encode_frame_4ch>
                              : WriteOutput<PLOYTEC_OUT_LAYOUT_INT, 4, ploytec_encode_frame_4ch>;
    case 2: return bulkLayout ? WriteOutput<PLOYTEC_OUT_LAYOUT_BULK, 2, ploytec_encode_frame_2ch>
                              : WriteOutput<PLOYTEC_OUT_LAYOUT_INT, 2, ploytec_encode_frame_2ch>;
    default: return nullptr;
    }
}

ReadInputFunc PloytecInputReader(uint32_t channels) {
    switch (channels) {
    case 2: return ReadInput<2, ploytec_decode_frame_2ch>;
    case 4: return ReadInput<4, ploytec_decode_frame_4ch>;
    case 6: return ReadInput<6, ploytec_decode_frame_6ch>;
    case 8: return ReadInput<8, ploytec_decode_frame_8ch>;
    case 10: return ReadInput<10, ploytec_decode_frame_10ch>;
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
