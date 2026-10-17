#ifndef PloytecCodec_h
#define PloytecCodec_h

#include <stdint.h>

// Ploytec PCM encoding/decoding for the HAL (float), on the float codecs in common/devices/ploytec/ploytec_codec.c
// Up to 16 output / 16 input channels of 24-bit audio in the bit-interleaved junction frame

// Packet-level I/O operations (handle MIDI byte interleaving)
typedef void (*WriteOutputFunc)(uint8_t* ringBuffer, const float* srcFrames, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize);
typedef void (*ReadInputFunc)(float* dstFrames, const uint8_t* ringBuffer, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize);
typedef void (*ClearOutputFunc)(uint8_t* outputBuffer, uint32_t bufferSize);

// Writer/reader for a channel count, picked once per device (nullptr if there is no codec for it).
// The channel count is a template parameter inside, so the I/O path doesn't branch on it.
// spdifMirror: the ESU1808's 8 channels plus the S/PDIF mirror of pair mirrorPair (0 = 1/2; bulk
// layout only)
WriteOutputFunc PloytecOutputWriter(bool bulkLayout, uint32_t channels, bool spdifMirror = false,
                                    unsigned int mirrorPair = 0);
ReadInputFunc PloytecInputReader(uint32_t channels);

// Plain output (kOzzyDeviceFlagPlainOutput): interleaved S24_3LE frames, no framing
WriteOutputFunc PloytecPlainOutputWriter(uint32_t channels);

void PloytecClearOutputBulk(uint8_t* outputBuffer, uint32_t bufferSize);
void PloytecClearOutputInterrupt(uint8_t* outputBuffer, uint32_t bufferSize);
void PloytecClearOutputPlain(uint8_t* outputBuffer, uint32_t bufferSize);

#endif
