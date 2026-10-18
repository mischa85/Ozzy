#ifndef PloytecCodec_h
#define PloytecCodec_h

#include <stdint.h>

// Ploytec PCM encoding/decoding for the HAL (float), on the float codecs in common/devices/ploytec/ploytec_codec.c
// Up to 16 output / 16 input channels of 24-bit audio in the bit-interleaved junction frame

// The shared memory's rings as the transfers go on the wire, frame f at f % ringFrames: output in
// sub-packets of 10 frames in the device's layout (with their MIDI and control bytes), input in
// 512-byte sub-packets of 8 frames (engine/shared/ozzy_shared_data.h)
typedef void (*WriteOutputFunc)(uint8_t* ringBuffer, const float* srcFrames, uint64_t sampleTime, uint32_t frameCount, uint32_t ringFrames);
typedef void (*ReadInputFunc)(float* dstFrames, const uint8_t* ringBuffer, uint64_t sampleTime, uint32_t frameCount, uint32_t ringFrames);
typedef void (*ClearOutputFunc)(uint8_t* outputBuffer, uint32_t ringFrames);

// Writer/reader for a channel count, picked once per device (nullptr if there is no codec for it).
// The channel count is a template parameter inside, so the I/O path doesn't branch on it.
// spdifMirror: the ESU1808's 8 channels plus the S/PDIF mirror of pair mirrorPair (0 = 1/2; bulk
// layout only)
WriteOutputFunc PloytecOutputWriter(bool bulkLayout, uint32_t channels, bool spdifMirror = false,
                                    unsigned int mirrorPair = 0);
ReadInputFunc PloytecInputReader(uint32_t channels);

// Plain output (OZZY_DEVICE_FLAG_PLAIN_OUTPUT): interleaved S24_3LE frames, no framing
WriteOutputFunc PloytecPlainOutputWriter(uint32_t channels);

void PloytecClearOutputBulk(uint8_t* outputBuffer, uint32_t ringFrames);
void PloytecClearOutputInterrupt(uint8_t* outputBuffer, uint32_t ringFrames);
void PloytecClearOutputPlain(uint8_t* outputBuffer, uint32_t ringFrames);

#endif
