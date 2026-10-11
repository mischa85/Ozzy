#ifndef PloytecCodec_h
#define PloytecCodec_h

#include <stdint.h>

// Ploytec-specific PCM encoding/decoding
// Up to 8 output / 16 input channels of 24-bit audio in the bit-interleaved junction frame

void PloytecEncodePCM(uint8_t* dst, const float* src, uint32_t channels);
void PloytecDecodePCM(float* dst, const uint8_t* src, uint32_t channels);

// Packet-level I/O operations (handle MIDI byte interleaving)
typedef void (*WriteOutputFunc)(uint8_t* ringBuffer, const float* srcFrames, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize, uint32_t bytesPerFrame, uint32_t channels);
typedef void (*ReadInputFunc)(float* dstFrames, const uint8_t* ringBuffer, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize, uint32_t bytesPerFrame, uint32_t channels);
typedef void (*ClearOutputFunc)(uint8_t* outputBuffer, uint32_t bufferSize);

void PloytecWriteOutputBulk(uint8_t* ringBuffer, const float* srcFrames, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize, uint32_t bytesPerFrame, uint32_t channels);
void PloytecWriteOutputInterrupt(uint8_t* ringBuffer, const float* srcFrames, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize, uint32_t bytesPerFrame, uint32_t channels);
void PloytecReadInput(float* dstFrames, const uint8_t* ringBuffer, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize, uint32_t bytesPerFrame, uint32_t channels);

void PloytecClearOutputBulk(uint8_t* outputBuffer, uint32_t bufferSize);
void PloytecClearOutputInterrupt(uint8_t* outputBuffer, uint32_t bufferSize);

#endif
