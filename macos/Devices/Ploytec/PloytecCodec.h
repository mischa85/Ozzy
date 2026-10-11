#ifndef PloytecCodec_h
#define PloytecCodec_h

#include <stdint.h>

// Ploytec-specific PCM encoding/decoding
// Up to 8 output / 16 input channels of 24-bit audio in the bit-interleaved junction frame

// Packet-level I/O operations (handle MIDI byte interleaving)
typedef void (*WriteOutputFunc)(uint8_t* ringBuffer, const float* srcFrames, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize, uint32_t bytesPerFrame);
typedef void (*ReadInputFunc)(float* dstFrames, const uint8_t* ringBuffer, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize, uint32_t bytesPerFrame);
typedef void (*ClearOutputFunc)(uint8_t* outputBuffer, uint32_t bufferSize);

// Writer/reader for a channel count, picked once per device (nullptr if there is no codec for it).
// The channel count is a template parameter inside, so the I/O path doesn't branch on it.
WriteOutputFunc PloytecOutputWriter(bool bulkLayout, uint32_t channels);
ReadInputFunc PloytecInputReader(uint32_t channels);

void PloytecClearOutputBulk(uint8_t* outputBuffer, uint32_t bufferSize);
void PloytecClearOutputInterrupt(uint8_t* outputBuffer, uint32_t bufferSize);

#endif
