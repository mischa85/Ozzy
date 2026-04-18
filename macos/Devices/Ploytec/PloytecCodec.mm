#include "PloytecCodec.h"
#include "../../Shared/OzzySharedData.h"
#include <cstring>

/* Encode 8 float channels into 48 bytes using the Ploytec bit-scatter wire format.
 * Matches ploytec_encode_frame() in common/devices/ploytec/ploytec_codec.c.
 *
 * Step 1: Convert 8 floats → 24 bytes S24_3LE (ch*3+0=LSB, ch*3+2=MSB).
 * Step 2: Bit-scatter 4 channels per output byte across 48 output bytes:
 *   Bytes 0x00-0x17: channels 0,2,4,6 (odd in 1-indexed Ploytec naming)
 *   Bytes 0x18-0x2F: channels 1,3,5,7 (even in 1-indexed Ploytec naming)
 * Each output byte holds one bit from each of 4 channels, MSB-first. */
void PloytecEncodePCM(uint8_t* dst, const float* src) {
    uint8_t s[24];
    for (int ch = 0; ch < 8; ch++) {
        float f = src[ch];
        if (f >  1.0f) f =  1.0f;
        if (f < -1.0f) f = -1.0f;
        int32_t v = (int32_t)(f * 8388608.0f);
        s[ch*3+0] = (uint8_t)(v & 0xFF);
        s[ch*3+1] = (uint8_t)((v >> 8) & 0xFF);
        s[ch*3+2] = (uint8_t)((v >> 16) & 0xFF);
    }
    /* Channels 0,2,4,6 → bytes 0x00-0x17 */
    dst[0x00]=((s[0x02]&0x80)>>7)|((s[0x08]&0x80)>>6)|((s[0x0E]&0x80)>>5)|((s[0x14]&0x80)>>4);
    dst[0x01]=((s[0x02]&0x40)>>6)|((s[0x08]&0x40)>>5)|((s[0x0E]&0x40)>>4)|((s[0x14]&0x40)>>3);
    dst[0x02]=((s[0x02]&0x20)>>5)|((s[0x08]&0x20)>>4)|((s[0x0E]&0x20)>>3)|((s[0x14]&0x20)>>2);
    dst[0x03]=((s[0x02]&0x10)>>4)|((s[0x08]&0x10)>>3)|((s[0x0E]&0x10)>>2)|((s[0x14]&0x10)>>1);
    dst[0x04]=((s[0x02]&0x08)>>3)|((s[0x08]&0x08)>>2)|((s[0x0E]&0x08)>>1)|((s[0x14]&0x08)>>0);
    dst[0x05]=((s[0x02]&0x04)>>2)|((s[0x08]&0x04)>>1)|((s[0x0E]&0x04)>>0)|((s[0x14]&0x04)<<1);
    dst[0x06]=((s[0x02]&0x02)>>1)|((s[0x08]&0x02)>>0)|((s[0x0E]&0x02)<<1)|((s[0x14]&0x02)<<2);
    dst[0x07]=((s[0x02]&0x01)>>0)|((s[0x08]&0x01)<<1)|((s[0x0E]&0x01)<<2)|((s[0x14]&0x01)<<3);
    dst[0x08]=((s[0x01]&0x80)>>7)|((s[0x07]&0x80)>>6)|((s[0x0D]&0x80)>>5)|((s[0x13]&0x80)>>4);
    dst[0x09]=((s[0x01]&0x40)>>6)|((s[0x07]&0x40)>>5)|((s[0x0D]&0x40)>>4)|((s[0x13]&0x40)>>3);
    dst[0x0A]=((s[0x01]&0x20)>>5)|((s[0x07]&0x20)>>4)|((s[0x0D]&0x20)>>3)|((s[0x13]&0x20)>>2);
    dst[0x0B]=((s[0x01]&0x10)>>4)|((s[0x07]&0x10)>>3)|((s[0x0D]&0x10)>>2)|((s[0x13]&0x10)>>1);
    dst[0x0C]=((s[0x01]&0x08)>>3)|((s[0x07]&0x08)>>2)|((s[0x0D]&0x08)>>1)|((s[0x13]&0x08)>>0);
    dst[0x0D]=((s[0x01]&0x04)>>2)|((s[0x07]&0x04)>>1)|((s[0x0D]&0x04)>>0)|((s[0x13]&0x04)<<1);
    dst[0x0E]=((s[0x01]&0x02)>>1)|((s[0x07]&0x02)>>0)|((s[0x0D]&0x02)<<1)|((s[0x13]&0x02)<<2);
    dst[0x0F]=((s[0x01]&0x01)>>0)|((s[0x07]&0x01)<<1)|((s[0x0D]&0x01)<<2)|((s[0x13]&0x01)<<3);
    dst[0x10]=((s[0x00]&0x80)>>7)|((s[0x06]&0x80)>>6)|((s[0x0C]&0x80)>>5)|((s[0x12]&0x80)>>4);
    dst[0x11]=((s[0x00]&0x40)>>6)|((s[0x06]&0x40)>>5)|((s[0x0C]&0x40)>>4)|((s[0x12]&0x40)>>3);
    dst[0x12]=((s[0x00]&0x20)>>5)|((s[0x06]&0x20)>>4)|((s[0x0C]&0x20)>>3)|((s[0x12]&0x20)>>2);
    dst[0x13]=((s[0x00]&0x10)>>4)|((s[0x06]&0x10)>>3)|((s[0x0C]&0x10)>>2)|((s[0x12]&0x10)>>1);
    dst[0x14]=((s[0x00]&0x08)>>3)|((s[0x06]&0x08)>>2)|((s[0x0C]&0x08)>>1)|((s[0x12]&0x08)>>0);
    dst[0x15]=((s[0x00]&0x04)>>2)|((s[0x06]&0x04)>>1)|((s[0x0C]&0x04)>>0)|((s[0x12]&0x04)<<1);
    dst[0x16]=((s[0x00]&0x02)>>1)|((s[0x06]&0x02)>>0)|((s[0x0C]&0x02)<<1)|((s[0x12]&0x02)<<2);
    dst[0x17]=((s[0x00]&0x01)>>0)|((s[0x06]&0x01)<<1)|((s[0x0C]&0x01)<<2)|((s[0x12]&0x01)<<3);
    /* Channels 1,3,5,7 → bytes 0x18-0x2F */
    dst[0x18]=((s[0x05]&0x80)>>7)|((s[0x0B]&0x80)>>6)|((s[0x11]&0x80)>>5)|((s[0x17]&0x80)>>4);
    dst[0x19]=((s[0x05]&0x40)>>6)|((s[0x0B]&0x40)>>5)|((s[0x11]&0x40)>>4)|((s[0x17]&0x40)>>3);
    dst[0x1A]=((s[0x05]&0x20)>>5)|((s[0x0B]&0x20)>>4)|((s[0x11]&0x20)>>3)|((s[0x17]&0x20)>>2);
    dst[0x1B]=((s[0x05]&0x10)>>4)|((s[0x0B]&0x10)>>3)|((s[0x11]&0x10)>>2)|((s[0x17]&0x10)>>1);
    dst[0x1C]=((s[0x05]&0x08)>>3)|((s[0x0B]&0x08)>>2)|((s[0x11]&0x08)>>1)|((s[0x17]&0x08)>>0);
    dst[0x1D]=((s[0x05]&0x04)>>2)|((s[0x0B]&0x04)>>1)|((s[0x11]&0x04)>>0)|((s[0x17]&0x04)<<1);
    dst[0x1E]=((s[0x05]&0x02)>>1)|((s[0x0B]&0x02)>>0)|((s[0x11]&0x02)<<1)|((s[0x17]&0x02)<<2);
    dst[0x1F]=((s[0x05]&0x01)>>0)|((s[0x0B]&0x01)<<1)|((s[0x11]&0x01)<<2)|((s[0x17]&0x01)<<3);
    dst[0x20]=((s[0x04]&0x80)>>7)|((s[0x0A]&0x80)>>6)|((s[0x10]&0x80)>>5)|((s[0x16]&0x80)>>4);
    dst[0x21]=((s[0x04]&0x40)>>6)|((s[0x0A]&0x40)>>5)|((s[0x10]&0x40)>>4)|((s[0x16]&0x40)>>3);
    dst[0x22]=((s[0x04]&0x20)>>5)|((s[0x0A]&0x20)>>4)|((s[0x10]&0x20)>>3)|((s[0x16]&0x20)>>2);
    dst[0x23]=((s[0x04]&0x10)>>4)|((s[0x0A]&0x10)>>3)|((s[0x10]&0x10)>>2)|((s[0x16]&0x10)>>1);
    dst[0x24]=((s[0x04]&0x08)>>3)|((s[0x0A]&0x08)>>2)|((s[0x10]&0x08)>>1)|((s[0x16]&0x08)>>0);
    dst[0x25]=((s[0x04]&0x04)>>2)|((s[0x0A]&0x04)>>1)|((s[0x10]&0x04)>>0)|((s[0x16]&0x04)<<1);
    dst[0x26]=((s[0x04]&0x02)>>1)|((s[0x0A]&0x02)>>0)|((s[0x10]&0x02)<<1)|((s[0x16]&0x02)<<2);
    dst[0x27]=((s[0x04]&0x01)>>0)|((s[0x0A]&0x01)<<1)|((s[0x10]&0x01)<<2)|((s[0x16]&0x01)<<3);
    dst[0x28]=((s[0x03]&0x80)>>7)|((s[0x09]&0x80)>>6)|((s[0x0F]&0x80)>>5)|((s[0x15]&0x80)>>4);
    dst[0x29]=((s[0x03]&0x40)>>6)|((s[0x09]&0x40)>>5)|((s[0x0F]&0x40)>>4)|((s[0x15]&0x40)>>3);
    dst[0x2A]=((s[0x03]&0x20)>>5)|((s[0x09]&0x20)>>4)|((s[0x0F]&0x20)>>3)|((s[0x15]&0x20)>>2);
    dst[0x2B]=((s[0x03]&0x10)>>4)|((s[0x09]&0x10)>>3)|((s[0x0F]&0x10)>>2)|((s[0x15]&0x10)>>1);
    dst[0x2C]=((s[0x03]&0x08)>>3)|((s[0x09]&0x08)>>2)|((s[0x0F]&0x08)>>1)|((s[0x15]&0x08)>>0);
    dst[0x2D]=((s[0x03]&0x04)>>2)|((s[0x09]&0x04)>>1)|((s[0x0F]&0x04)>>0)|((s[0x15]&0x04)<<1);
    dst[0x2E]=((s[0x03]&0x02)>>1)|((s[0x09]&0x02)>>0)|((s[0x0F]&0x02)<<1)|((s[0x15]&0x02)<<2);
    dst[0x2F]=((s[0x03]&0x01)>>0)|((s[0x09]&0x01)<<1)|((s[0x0F]&0x01)<<2)|((s[0x15]&0x01)<<3);
}

void PloytecDecodePCM(float* dst, const uint8_t* src) {
    uint8_t c[8][3];
    c[0][2] = ((src[0x00] & 0x01) << 7) | ((src[0x01] & 0x01) << 6) | ((src[0x02] & 0x01) << 5) | ((src[0x03] & 0x01) << 4) | ((src[0x04] & 0x01) << 3) | ((src[0x05] & 0x01) << 2) | ((src[0x06] & 0x01) << 1) | ((src[0x07] & 0x01));
    c[0][1] = ((src[0x08] & 0x01) << 7) | ((src[0x09] & 0x01) << 6) | ((src[0x0A] & 0x01) << 5) | ((src[0x0B] & 0x01) << 4) | ((src[0x0C] & 0x01) << 3) | ((src[0x0D] & 0x01) << 2) | ((src[0x0E] & 0x01) << 1) | ((src[0x0F] & 0x01));
    c[0][0] = ((src[0x10] & 0x01) << 7) | ((src[0x11] & 0x01) << 6) | ((src[0x12] & 0x01) << 5) | ((src[0x13] & 0x01) << 4) | ((src[0x14] & 0x01) << 3) | ((src[0x15] & 0x01) << 2) | ((src[0x16] & 0x01) << 1) | ((src[0x17] & 0x01));
    c[2][2] = ((src[0x00] & 0x02) << 6) | ((src[0x01] & 0x02) << 5) | ((src[0x02] & 0x02) << 4) | ((src[0x03] & 0x02) << 3) | ((src[0x04] & 0x02) << 2) | ((src[0x05] & 0x02) << 1) | ((src[0x06] & 0x02)) | ((src[0x07] & 0x02) >> 1);
    c[2][1] = ((src[0x08] & 0x02) << 6) | ((src[0x09] & 0x02) << 5) | ((src[0x0A] & 0x02) << 4) | ((src[0x0B] & 0x02) << 3) | ((src[0x0C] & 0x02) << 2) | ((src[0x0D] & 0x02) << 1) | ((src[0x0E] & 0x02)) | ((src[0x0F] & 0x02) >> 1);
    c[2][0] = ((src[0x10] & 0x02) << 6) | ((src[0x11] & 0x02) << 5) | ((src[0x12] & 0x02) << 4) | ((src[0x13] & 0x02) << 3) | ((src[0x14] & 0x02) << 2) | ((src[0x15] & 0x02) << 1) | ((src[0x16] & 0x02)) | ((src[0x17] & 0x02) >> 1);
    c[4][2] = ((src[0x00] & 0x04) << 5) | ((src[0x01] & 0x04) << 4) | ((src[0x02] & 0x04) << 3) | ((src[0x03] & 0x04) << 2) | ((src[0x04] & 0x04) << 1) | ((src[0x05] & 0x04)) | ((src[0x06] & 0x04) >> 1) | ((src[0x07] & 0x04) >> 2);
    c[4][1] = ((src[0x08] & 0x04) << 5) | ((src[0x09] & 0x04) << 4) | ((src[0x0A] & 0x04) << 3) | ((src[0x0B] & 0x04) << 2) | ((src[0x0C] & 0x04) << 1) | ((src[0x0D] & 0x04)) | ((src[0x0E] & 0x04) >> 1) | ((src[0x0F] & 0x04) >> 2);
    c[4][0] = ((src[0x10] & 0x04) << 5) | ((src[0x11] & 0x04) << 4) | ((src[0x12] & 0x04) << 3) | ((src[0x13] & 0x04) << 2) | ((src[0x14] & 0x04) << 1) | ((src[0x15] & 0x04)) | ((src[0x16] & 0x04) >> 1) | ((src[0x17] & 0x04) >> 2);
    c[6][2] = ((src[0x00] & 0x08) << 4) | ((src[0x01] & 0x08) << 3) | ((src[0x02] & 0x08) << 2) | ((src[0x03] & 0x08) << 1) | ((src[0x04] & 0x08)) | ((src[0x05] & 0x08) >> 1) | ((src[0x06] & 0x08) >> 2) | ((src[0x07] & 0x08) >> 3);
    c[6][1] = ((src[0x08] & 0x08) << 4) | ((src[0x09] & 0x08) << 3) | ((src[0x0A] & 0x08) << 2) | ((src[0x0B] & 0x08) << 1) | ((src[0x0C] & 0x08)) | ((src[0x0D] & 0x08) >> 1) | ((src[0x0E] & 0x08) >> 2) | ((src[0x0F] & 0x08) >> 3);
    c[6][0] = ((src[0x10] & 0x08) << 4) | ((src[0x11] & 0x08) << 3) | ((src[0x12] & 0x08) << 2) | ((src[0x13] & 0x08) << 1) | ((src[0x14] & 0x08)) | ((src[0x15] & 0x08) >> 1) | ((src[0x16] & 0x08) >> 2) | ((src[0x17] & 0x08) >> 3);
    c[1][2] = ((src[0x20] & 0x01) << 7) | ((src[0x21] & 0x01) << 6) | ((src[0x22] & 0x01) << 5) | ((src[0x23] & 0x01) << 4) | ((src[0x24] & 0x01) << 3) | ((src[0x25] & 0x01) << 2) | ((src[0x26] & 0x01) << 1) | ((src[0x27] & 0x01));
    c[1][1] = ((src[0x28] & 0x01) << 7) | ((src[0x29] & 0x01) << 6) | ((src[0x2A] & 0x01) << 5) | ((src[0x2B] & 0x01) << 4) | ((src[0x2C] & 0x01) << 3) | ((src[0x2D] & 0x01) << 2) | ((src[0x2E] & 0x01) << 1) | ((src[0x2F] & 0x01));
    c[1][0] = ((src[0x30] & 0x01) << 7) | ((src[0x31] & 0x01) << 6) | ((src[0x32] & 0x01) << 5) | ((src[0x33] & 0x01) << 4) | ((src[0x34] & 0x01) << 3) | ((src[0x35] & 0x01) << 2) | ((src[0x36] & 0x01) << 1) | ((src[0x37] & 0x01));
    c[3][2] = ((src[0x20] & 0x02) << 6) | ((src[0x21] & 0x02) << 5) | ((src[0x22] & 0x02) << 4) | ((src[0x23] & 0x02) << 3) | ((src[0x24] & 0x02) << 2) | ((src[0x25] & 0x02) << 1) | ((src[0x26] & 0x02)) | ((src[0x27] & 0x02) >> 1);
    c[3][1] = ((src[0x28] & 0x02) << 6) | ((src[0x29] & 0x02) << 5) | ((src[0x2A] & 0x02) << 4) | ((src[0x2B] & 0x02) << 3) | ((src[0x2C] & 0x02) << 2) | ((src[0x2D] & 0x02) << 1) | ((src[0x2E] & 0x02)) | ((src[0x2F] & 0x02) >> 1);
    c[3][0] = ((src[0x30] & 0x02) << 6) | ((src[0x31] & 0x02) << 5) | ((src[0x32] & 0x02) << 4) | ((src[0x33] & 0x02) << 3) | ((src[0x34] & 0x02) << 2) | ((src[0x35] & 0x02) << 1) | ((src[0x36] & 0x02)) | ((src[0x37] & 0x02) >> 1);
    c[5][2] = ((src[0x20] & 0x04) << 5) | ((src[0x21] & 0x04) << 4) | ((src[0x22] & 0x04) << 3) | ((src[0x23] & 0x04) << 2) | ((src[0x24] & 0x04) << 1) | ((src[0x25] & 0x04)) | ((src[0x26] & 0x04) >> 1) | ((src[0x27] & 0x04) >> 2);
    c[5][1] = ((src[0x28] & 0x04) << 5) | ((src[0x29] & 0x04) << 4) | ((src[0x2A] & 0x04) << 3) | ((src[0x2B] & 0x04) << 2) | ((src[0x2C] & 0x04) << 1) | ((src[0x2D] & 0x04)) | ((src[0x2E] & 0x04) >> 1) | ((src[0x2F] & 0x04) >> 2);
    c[5][0] = ((src[0x30] & 0x04) << 5) | ((src[0x31] & 0x04) << 4) | ((src[0x32] & 0x04) << 3) | ((src[0x33] & 0x04) << 2) | ((src[0x34] & 0x04) << 1) | ((src[0x35] & 0x04)) | ((src[0x36] & 0x04) >> 1) | ((src[0x37] & 0x04) >> 2);
    c[7][2] = ((src[0x20] & 0x08) << 4) | ((src[0x21] & 0x08) << 3) | ((src[0x22] & 0x08) << 2) | ((src[0x23] & 0x08) << 1) | ((src[0x24] & 0x08)) | ((src[0x25] & 0x08) >> 1) | ((src[0x26] & 0x08) >> 2) | ((src[0x27] & 0x08) >> 3);
    c[7][1] = ((src[0x28] & 0x08) << 4) | ((src[0x29] & 0x08) << 3) | ((src[0x2A] & 0x08) << 2) | ((src[0x2B] & 0x08) << 1) | ((src[0x2C] & 0x08)) | ((src[0x2D] & 0x08) >> 1) | ((src[0x2E] & 0x08) >> 2) | ((src[0x2F] & 0x08) >> 3);
    c[7][0] = ((src[0x30] & 0x08) << 4) | ((src[0x31] & 0x08) << 3) | ((src[0x32] & 0x08) << 2) | ((src[0x33] & 0x08) << 1) | ((src[0x34] & 0x08)) | ((src[0x35] & 0x08) >> 1) | ((src[0x36] & 0x08) >> 2) | ((src[0x37] & 0x08) >> 3);

    for(int i=0; i<8; i++) {
        int32_t s = (c[i][0]) | (c[i][1] << 8) | (c[i][2] << 16);
        if(s & 0x800000) s |= 0xFF000000;
        dst[i] = (float)s / 8388608.0f;
    }
}

// BULK Mode: Ring buffer mirrors USB packet structure for zero-copy
// Packet structure: [480 bytes PCM (10 samples)][2 bytes MIDI][30 bytes padding] = 512 bytes/packet
void PloytecWriteOutputBulk(uint8_t* ringBuffer, const float* srcFrames, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize, uint32_t bytesPerFrame) {
    for (uint32_t i = 0; i < frameCount; i++) {
        uint32_t sampleOffset = (uint32_t)((sampleTime + i) % ringSize);
        
        // Each logical packet = 80 frames = 8 USB sub-packets of 10 frames each
        uint32_t logicalPacket = sampleOffset / 80;
        uint32_t frameInLogicalPacket = sampleOffset % 80;
        uint32_t usbSubPacket = frameInLogicalPacket / 10;
        uint32_t sampleInSubPacket = frameInLogicalPacket % 10;
        
        // Calculate byte address: logical packet base + USB sub-packet offset + sample offset
        uint32_t byteOffset = (logicalPacket * kOzzyMaxPacketSize) + (usbSubPacket * 512) + (sampleInSubPacket * bytesPerFrame);
        PloytecEncodePCM(ringBuffer + byteOffset, srcFrames + (i * 8));
    }
}

// INTERRUPT Mode: Ring buffer mirrors USB packet structure for zero-copy
// Wire format confirmed by USB capture: same as bulk — [480 bytes PCM][2 bytes MIDI][30 bytes padding] = 512 bytes/packet
void PloytecWriteOutputInterrupt(uint8_t* ringBuffer, const float* srcFrames, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize, uint32_t bytesPerFrame) {
    for (uint32_t i = 0; i < frameCount; i++) {
        uint32_t sampleOffset = (uint32_t)((sampleTime + i) % ringSize);

        // Each logical packet = 80 frames = 8 USB sub-packets of 10 frames each
        uint32_t logicalPacket = sampleOffset / 80;
        uint32_t frameInLogicalPacket = sampleOffset % 80;
        uint32_t usbSubPacket = frameInLogicalPacket / 10;
        uint32_t sampleInSubPacket = frameInLogicalPacket % 10;

        // All 10 samples are contiguous before MIDI bytes
        uint32_t sampleByteOffset = sampleInSubPacket * bytesPerFrame;

        uint32_t byteOffset = (logicalPacket * kOzzyMaxPacketSize) + (usbSubPacket * 512) + sampleByteOffset;
        PloytecEncodePCM(ringBuffer + byteOffset, srcFrames + (i * 8));
    }
}

// Read input samples from ring buffer
// Ring buffer layout: logical packets at kOzzyMaxPacketSize stride, no MIDI in input
void PloytecReadInput(float* dstFrames, const uint8_t* ringBuffer, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize, uint32_t bytesPerFrame) {
    for (uint32_t i = 0; i < frameCount; i++) {
        uint32_t sampleOffset = (uint32_t)((sampleTime + i) % ringSize);
        
        // Input is simpler - no MIDI interleaving, just linear samples within logical packets
        uint32_t logicalPacket = sampleOffset / 80;
        uint32_t frameInLogicalPacket = sampleOffset % 80;
        
        uint32_t byteOffset = (logicalPacket * kOzzyMaxPacketSize) + (frameInLogicalPacket * bytesPerFrame);
        PloytecDecodePCM(dstFrames + (i * 8), ringBuffer + byteOffset);
    }
}

// Clear output buffer - BULK mode: Clear PCM samples only, preserve MIDI byte positions
// Ring buffer layout: 128 logical packets at kOzzyMaxPacketSize stride
// Each logical packet contains 8 USB sub-packets
// Packet structure: [480 bytes PCM][2 bytes MIDI][30 bytes padding] = 512 bytes/packet
void PloytecClearOutputBulk(uint8_t* outputBuffer, uint32_t bufferSize) {
    const uint32_t usbPacketSize = 512;
    const uint32_t pcmSize = 480;
    const uint32_t numLogicalPackets = kOzzyNumPackets;  // 128 logical packets
    const uint32_t usbSubPacketsPerLogical = 8;
    
    for (uint32_t logicalPacket = 0; logicalPacket < numLogicalPackets; logicalPacket++) {
        uint32_t logicalPacketBase = logicalPacket * kOzzyMaxPacketSize;
        
        for (uint32_t subPacket = 0; subPacket < usbSubPacketsPerLogical; subPacket++) {
            uint8_t* usbPacket = outputBuffer + logicalPacketBase + (subPacket * usbPacketSize);
            memset(usbPacket, 0, pcmSize);  // Clear PCM bytes 0-479
            // Leave MIDI bytes 480-481 untouched
            memset(usbPacket + 482, 0, 30); // Clear padding bytes 482-511
        }
    }
}

// Clear output buffer - INTERRUPT mode: Clear PCM samples only, preserve MIDI byte positions
// Ring buffer layout: 128 logical packets at kOzzyMaxPacketSize stride
// Each logical packet contains 8 USB sub-packets
// Packet structure: [480 bytes PCM (10 samples)][2 bytes MIDI][30 bytes padding] = 512 bytes/packet
void PloytecClearOutputInterrupt(uint8_t* outputBuffer, uint32_t bufferSize) {
    const uint32_t usbPacketSize = 512;
    const uint32_t pcmSize = 480;
    const uint32_t numLogicalPackets = kOzzyNumPackets;  // 128 logical packets
    const uint32_t usbSubPacketsPerLogical = 8;

    for (uint32_t logicalPacket = 0; logicalPacket < numLogicalPackets; logicalPacket++) {
        uint32_t logicalPacketBase = logicalPacket * kOzzyMaxPacketSize;

        for (uint32_t subPacket = 0; subPacket < usbSubPacketsPerLogical; subPacket++) {
            uint8_t* usbPacket = outputBuffer + logicalPacketBase + (subPacket * usbPacketSize);
            memset(usbPacket, 0, pcmSize);  // Clear PCM bytes 0-479 (10 samples)
            // Leave MIDI bytes 480-481 untouched
            memset(usbPacket + 482, 0, 30); // Clear padding bytes 482-511
        }
    }
}
