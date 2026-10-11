#include "PloytecCodec.h"
#include "PloytecProtocol.h"
#include "../../../common/devices/ploytec/ploytec_codec.h"
#include "../../Shared/OzzySharedData.h"
#include <cstring>

// The bit-interleaving itself is the shared codec (common/devices/ploytec/ploytec_codec.c),
// which works on S24_3LE. The HAL works in float, so convert around it.
void PloytecEncodePCM(uint8_t* dst, const float* src, uint32_t channels) {
    uint8_t s24[PLOYTEC_MAX_OUT_CHANNELS * 3];
    for (uint32_t i = 0; i < channels; i++) {
        int32_t s = (int32_t)(src[i] * 8388608.0f);
        if (src[i] > 1.0f) s = 0x7FFFFF; else if (src[i] < -1.0f) s = -0x800000;
        s24[i * 3 + 0] = s & 0xFF;
        s24[i * 3 + 1] = (s >> 8) & 0xFF;
        s24[i * 3 + 2] = (s >> 16) & 0xFF;
    }
    ploytec_encode_frame(dst, s24, channels);
}

void PloytecDecodePCM(float* dst, const uint8_t* src, uint32_t channels) {
    uint8_t s24[PLOYTEC_MAX_IN_CHANNELS * 3];
    ploytec_decode_frame(s24, src, channels);
    for (uint32_t i = 0; i < channels; i++) {
        int32_t s = s24[i * 3] | (s24[i * 3 + 1] << 8) | (s24[i * 3 + 2] << 16);
        if (s & 0x800000) s |= 0xFF000000;
        dst[i] = (float)s / 8388608.0f;
    }
}

// Ring buffer mirrors the USB packet structure for zero-copy: logical packets of 80 frames at
// kOzzyMaxPacketSize stride. Frame offsets inside a packet come from the shared layout helpers.
static void PloytecWriteOutput(enum ploytec_out_layout layout, uint8_t* ringBuffer, const float* srcFrames, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize, uint32_t channels) {
    for (uint32_t i = 0; i < frameCount; i++) {
        uint32_t sampleOffset = (uint32_t)((sampleTime + i) % ringSize);
        uint32_t logicalPacket = sampleOffset / PLOYTEC_FRAMES_PER_PKT;
        uint32_t frameInLogicalPacket = sampleOffset % PLOYTEC_FRAMES_PER_PKT;

        uint32_t byteOffset = (logicalPacket * kOzzyMaxPacketSize) + ploytec_out_frame_offset(layout, frameInLogicalPacket);
        PloytecEncodePCM(ringBuffer + byteOffset, srcFrames + (i * channels), channels);
    }
}

void PloytecWriteOutputBulk(uint8_t* ringBuffer, const float* srcFrames, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize, uint32_t bytesPerFrame, uint32_t channels) {
    PloytecWriteOutput(PLOYTEC_OUT_LAYOUT_BULK, ringBuffer, srcFrames, sampleTime, frameCount, ringSize, channels);
}

void PloytecWriteOutputInterrupt(uint8_t* ringBuffer, const float* srcFrames, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize, uint32_t bytesPerFrame, uint32_t channels) {
    PloytecWriteOutput(PLOYTEC_OUT_LAYOUT_INT, ringBuffer, srcFrames, sampleTime, frameCount, ringSize, channels);
}

// Read input samples from ring buffer
// Ring buffer layout: logical packets at kOzzyMaxPacketSize stride, no MIDI in input
void PloytecReadInput(float* dstFrames, const uint8_t* ringBuffer, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize, uint32_t bytesPerFrame, uint32_t channels) {
    for (uint32_t i = 0; i < frameCount; i++) {
        uint32_t sampleOffset = (uint32_t)((sampleTime + i) % ringSize);
        
        // Input is simpler - no MIDI interleaving, just linear samples within logical packets
        uint32_t logicalPacket = sampleOffset / 80;
        uint32_t frameInLogicalPacket = sampleOffset % 80;
        
        uint32_t byteOffset = (logicalPacket * kOzzyMaxPacketSize) + (frameInLogicalPacket * bytesPerFrame);
        PloytecDecodePCM(dstFrames + (i * channels), ringBuffer + byteOffset, channels);
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
