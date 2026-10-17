#include "PloytecCodec.h"
#include "../../engine/devices/ploytec/PloytecProtocol.h"
#include "../../common/devices/ploytec/ploytec_codec.h"
#include "../../engine/shared/OzzySharedData.h"
#include <cstring>

// The HAL works in float: the float frame codecs in common/devices/ploytec/ploytec_codec.c
// convert and bit-interleave in one call. The channel count and frame codec are template
// parameters, so the codec call is direct and the I/O path never branches on the channel count.

// Ring buffer mirrors the USB packet structure for zero-copy: logical packets of 80 frames at
// kOzzyMaxPacketSize stride. Frame offsets inside a packet come from the shared layout helpers.
template <enum ploytec_out_layout Layout, uint32_t Channels, ploytec_frame_f32_enc_fn Encode>
static void WriteOutput(uint8_t* ringBuffer, const float* srcFrames, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize) {
    for (uint32_t i = 0; i < frameCount; i++) {
        uint32_t sampleOffset = (uint32_t)((sampleTime + i) % ringSize);
        uint32_t logicalPacket = sampleOffset / PLOYTEC_FRAMES_PER_PKT;
        uint32_t frameInLogicalPacket = sampleOffset % PLOYTEC_FRAMES_PER_PKT;

        uint32_t byteOffset = (logicalPacket * kOzzyMaxPacketSize) + ploytec_out_frame_offset(Layout, frameInLogicalPacket);
        Encode(ringBuffer + byteOffset, srcFrames + (i * Channels));
    }
}

// The ESU1808 (bulk layout, 8 channels): the 8-channel frame plus the S/PDIF mirror of pair Pair,
// whose lane 9 is the pair's second channel of the frame before. That frame is in the ring
// already (written just before, or by the previous I/O cycle), so the mirror takes it from there.
template <unsigned int Pair>
static void WriteOutputSpdif(uint8_t* ringBuffer, const float* srcFrames, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize) {
    for (uint32_t i = 0; i < frameCount; i++) {
        uint32_t sampleOffset = (uint32_t)((sampleTime + i) % ringSize);
        uint32_t prevOffset = (sampleOffset ? sampleOffset : ringSize) - 1;
        uint8_t* dst = ringBuffer + (sampleOffset / PLOYTEC_FRAMES_PER_PKT) * kOzzyMaxPacketSize
                     + ploytec_out_frame_offset(PLOYTEC_OUT_LAYOUT_BULK, sampleOffset % PLOYTEC_FRAMES_PER_PKT);
        const uint8_t* prev = ringBuffer + (prevOffset / PLOYTEC_FRAMES_PER_PKT) * kOzzyMaxPacketSize
                            + ploytec_out_frame_offset(PLOYTEC_OUT_LAYOUT_BULK, prevOffset % PLOYTEC_FRAMES_PER_PKT);
        ploytec_encode_frame_8ch_f32(dst, srcFrames + (i * 8));
        ploytec_spdif_mirror(dst, prev + PLOYTEC_OUT_FRAME_SIZE / 2, Pair);
    }
}

// Read input samples from ring buffer
// Ring buffer layout: logical packets at kOzzyMaxPacketSize stride, no MIDI in input
template <uint32_t Channels, ploytec_frame_f32_dec_fn Decode>
static void ReadInput(float* dstFrames, const uint8_t* ringBuffer, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize) {
    for (uint32_t i = 0; i < frameCount; i++) {
        uint32_t sampleOffset = (uint32_t)((sampleTime + i) % ringSize);

        // Input is simpler - no MIDI interleaving, just linear samples within logical packets
        uint32_t logicalPacket = sampleOffset / PLOYTEC_FRAMES_PER_PKT;
        uint32_t frameInLogicalPacket = sampleOffset % PLOYTEC_FRAMES_PER_PKT;

        uint32_t byteOffset = (logicalPacket * kOzzyMaxPacketSize) + (frameInLogicalPacket * PLOYTEC_IN_FRAME_SIZE);
        Decode(dstFrames + (i * Channels), ringBuffer + byteOffset);
    }
}

// Plain output: a ring of @ringSize interleaved S24_3LE frames, frame f at f * Channels * 3,
// which the kext's isochronous transfers read directly
template <uint32_t Channels, ploytec_frame_f32_enc_fn Convert>
static void WriteOutputPlain(uint8_t* ringBuffer, const float* srcFrames, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize) {
    uint32_t f = (uint32_t)(sampleTime % ringSize);
    for (uint32_t i = 0; i < frameCount; i++) {
        Convert(ringBuffer + f * (Channels * 3), srcFrames + i * Channels);
        f++;
        f -= (f == ringSize) * ringSize;
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
    case 16: return bulkLayout ? WriteOutput<PLOYTEC_OUT_LAYOUT_BULK, 16, ploytec_encode_frame_16ch_f32>
                               : WriteOutput<PLOYTEC_OUT_LAYOUT_INT, 16, ploytec_encode_frame_16ch_f32>;
    case 10: return bulkLayout ? WriteOutput<PLOYTEC_OUT_LAYOUT_BULK, 10, ploytec_encode_frame_10ch_f32>
                               : WriteOutput<PLOYTEC_OUT_LAYOUT_INT, 10, ploytec_encode_frame_10ch_f32>;
    case 8: return bulkLayout ? WriteOutput<PLOYTEC_OUT_LAYOUT_BULK, 8, ploytec_encode_frame_8ch_f32>
                              : WriteOutput<PLOYTEC_OUT_LAYOUT_INT, 8, ploytec_encode_frame_8ch_f32>;
    case 6: return bulkLayout ? WriteOutput<PLOYTEC_OUT_LAYOUT_BULK, 6, ploytec_encode_frame_6ch_f32>
                              : WriteOutput<PLOYTEC_OUT_LAYOUT_INT, 6, ploytec_encode_frame_6ch_f32>;
    case 4: return bulkLayout ? WriteOutput<PLOYTEC_OUT_LAYOUT_BULK, 4, ploytec_encode_frame_4ch_f32>
                              : WriteOutput<PLOYTEC_OUT_LAYOUT_INT, 4, ploytec_encode_frame_4ch_f32>;
    case 2: return bulkLayout ? WriteOutput<PLOYTEC_OUT_LAYOUT_BULK, 2, ploytec_encode_frame_2ch_f32>
                              : WriteOutput<PLOYTEC_OUT_LAYOUT_INT, 2, ploytec_encode_frame_2ch_f32>;
    default: return nullptr;
    }
}

WriteOutputFunc PloytecPlainOutputWriter(uint32_t channels) {
    switch (channels) {
    case 2: return WriteOutputPlain<2, ploytec_f32_to_s24_2ch>;
    case 4: return WriteOutputPlain<4, ploytec_f32_to_s24_4ch>;
    case 6: return WriteOutputPlain<6, ploytec_f32_to_s24_6ch>;
    case 8: return WriteOutputPlain<8, ploytec_f32_to_s24_8ch>;
    default: return nullptr;
    }
}

ReadInputFunc PloytecInputReader(uint32_t channels) {
    switch (channels) {
    case 2: return ReadInput<2, ploytec_decode_frame_2ch_f32>;
    case 4: return ReadInput<4, ploytec_decode_frame_4ch_f32>;
    case 6: return ReadInput<6, ploytec_decode_frame_6ch_f32>;
    case 8: return ReadInput<8, ploytec_decode_frame_8ch_f32>;
    case 10: return ReadInput<10, ploytec_decode_frame_10ch_f32>;
    case 16: return ReadInput<16, ploytec_decode_frame_16ch_f32>;
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

// Silence: plain frames have no framing bytes to keep
void PloytecClearOutputPlain(uint8_t* outputBuffer, uint32_t bufferSize) {
    memset(outputBuffer, 0, bufferSize);
}
