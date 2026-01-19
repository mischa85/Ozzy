#include "PloytecCodec.h"
#include "../../Shared/OzzySharedData.h"
#include <cstring>

void PloytecEncodePCM(uint8_t* dst, const float* src) {
    int32_t s[8];
    for(int i=0; i<8; i++) {
        s[i] = (int32_t)(src[i] * 8388608.0f);
        if(src[i] > 1.0f) s[i] = 0x7FFFFF; else if(src[i] < -1.0f) s[i] = -0x800000;
    }
    uint8_t c[8][3];
    for(int i=0; i<8; i++) {
        c[i][0] = s[i] & 0xFF; c[i][1] = (s[i] >> 8) & 0xFF; c[i][2] = (s[i] >> 16) & 0xFF;
    }
    const uint8_t c1_L = c[0][0]; const uint8_t c1_M = c[0][1]; const uint8_t c1_H = c[0][2];
    const uint8_t c2_L = c[1][0]; const uint8_t c2_M = c[1][1]; const uint8_t c2_H = c[1][2];
    const uint8_t c3_L = c[2][0]; const uint8_t c3_M = c[2][1]; const uint8_t c3_H = c[2][2];
    const uint8_t c4_L = c[3][0]; const uint8_t c4_M = c[3][1]; const uint8_t c4_H = c[3][2];
    const uint8_t c5_L = c[4][0]; const uint8_t c5_M = c[4][1]; const uint8_t c5_H = c[4][2];
    const uint8_t c6_L = c[5][0]; const uint8_t c6_M = c[5][1]; const uint8_t c6_H = c[5][2];
    const uint8_t c7_L = c[6][0]; const uint8_t c7_M = c[6][1]; const uint8_t c7_H = c[6][2];
    const uint8_t c8_L = c[7][0]; const uint8_t c8_M = c[7][1]; const uint8_t c8_H = c[7][2];

    dst[0x00] = ((c1_H & 0x80) >> 0x07) | ((c3_H & 0x80) >> 0x06) | ((c5_H & 0x80) >> 0x05) | ((c7_H & 0x80) >> 0x04);
    dst[0x01] = ((c1_H & 0x40) >> 0x06) | ((c3_H & 0x40) >> 0x05) | ((c5_H & 0x40) >> 0x04) | ((c7_H & 0x40) >> 0x03);
    dst[0x02] = ((c1_H & 0x20) >> 0x05) | ((c3_H & 0x20) >> 0x04) | ((c5_H & 0x20) >> 0x03) | ((c7_H & 0x20) >> 0x02);
    dst[0x03] = ((c1_H & 0x10) >> 0x04) | ((c3_H & 0x10) >> 0x03) | ((c5_H & 0x10) >> 0x02) | ((c7_H & 0x10) >> 0x01);
    dst[0x04] = ((c1_H & 0x08) >> 0x03) | ((c3_H & 0x08) >> 0x02) | ((c5_H & 0x08) >> 0x01) | ((c7_H & 0x08) >> 0x00);
    dst[0x05] = ((c1_H & 0x04) >> 0x02) | ((c3_H & 0x04) >> 0x01) | ((c5_H & 0x04) >> 0x00) | ((c7_H & 0x04) << 0x01);
    dst[0x06] = ((c1_H & 0x02) >> 0x01) | ((c3_H & 0x02) >> 0x00) | ((c5_H & 0x02) << 0x01) | ((c7_H & 0x02) << 0x02);
    dst[0x07] = ((c1_H & 0x01) >> 0x00) | ((c3_H & 0x01) << 0x01) | ((c5_H & 0x01) << 0x02) | ((c7_H & 0x01) << 0x03);
    dst[0x08] = ((c1_M & 0x80) >> 0x07) | ((c3_M & 0x80) >> 0x06) | ((c5_M & 0x80) >> 0x05) | ((c7_M & 0x80) >> 0x04);
    dst[0x09] = ((c1_M & 0x40) >> 0x06) | ((c3_M & 0x40) >> 0x05) | ((c5_M & 0x40) >> 0x04) | ((c7_M & 0x40) >> 0x03);
    dst[0x0A] = ((c1_M & 0x20) >> 0x05) | ((c3_M & 0x20) >> 0x04) | ((c5_M & 0x20) >> 0x03) | ((c7_M & 0x20) >> 0x02);
    dst[0x0B] = ((c1_M & 0x10) >> 0x04) | ((c3_M & 0x10) >> 0x03) | ((c5_M & 0x10) >> 0x02) | ((c7_M & 0x10) >> 0x01);
    dst[0x0C] = ((c1_M & 0x08) >> 0x03) | ((c3_M & 0x08) >> 0x02) | ((c5_M & 0x08) >> 0x01) | ((c7_M & 0x08) >> 0x00);
    dst[0x0D] = ((c1_M & 0x04) >> 0x02) | ((c3_M & 0x04) >> 0x01) | ((c5_M & 0x04) >> 0x00) | ((c7_M & 0x04) << 0x01);
    dst[0x0E] = ((c1_M & 0x02) >> 0x01) | ((c3_M & 0x02) >> 0x00) | ((c5_M & 0x02) << 0x01) | ((c7_M & 0x02) << 0x02);
    dst[0x0F] = ((c1_M & 0x01) >> 0x00) | ((c3_M & 0x01) << 0x01) | ((c5_M & 0x01) << 0x02) | ((c7_M & 0x01) << 0x03);
    dst[0x10] = ((c1_L & 0x80) >> 0x07) | ((c3_L & 0x80) >> 0x06) | ((c5_L & 0x80) >> 0x05) | ((c7_L & 0x80) >> 0x04);
    dst[0x11] = ((c1_L & 0x40) >> 0x06) | ((c3_L & 0x40) >> 0x05) | ((c5_L & 0x40) >> 0x04) | ((c7_L & 0x40) >> 0x03);
    dst[0x12] = ((c1_L & 0x20) >> 0x05) | ((c3_L & 0x20) >> 0x04) | ((c5_L & 0x20) >> 0x03) | ((c7_L & 0x20) >> 0x02);
    dst[0x13] = ((c1_L & 0x10) >> 0x04) | ((c3_L & 0x10) >> 0x03) | ((c5_L & 0x10) >> 0x02) | ((c7_L & 0x10) >> 0x01);
    dst[0x14] = ((c1_L & 0x08) >> 0x03) | ((c3_L & 0x08) >> 0x02) | ((c5_L & 0x08) >> 0x01) | ((c7_L & 0x08) >> 0x00);
    dst[0x15] = ((c1_L & 0x04) >> 0x02) | ((c3_L & 0x04) >> 0x01) | ((c5_L & 0x04) >> 0x00) | ((c7_L & 0x04) << 0x01);
    dst[0x16] = ((c1_L & 0x02) >> 0x01) | ((c3_L & 0x02) >> 0x00) | ((c5_L & 0x02) << 0x01) | ((c7_L & 0x02) << 0x02);
    dst[0x17] = ((c1_L & 0x01) >> 0x00) | ((c3_L & 0x01) << 0x01) | ((c5_L & 0x01) << 0x02) | ((c7_L & 0x01) << 0x03);

    dst[0x18] = ((c2_H & 0x80) >> 0x07) | ((c4_H & 0x80) >> 0x06) | ((c6_H & 0x80) >> 0x05) | ((c8_H & 0x80) >> 0x04);
    dst[0x19] = ((c2_H & 0x40) >> 0x06) | ((c4_H & 0x40) >> 0x05) | ((c6_H & 0x40) >> 0x04) | ((c8_H & 0x40) >> 0x03);
    dst[0x1A] = ((c2_H & 0x20) >> 0x05) | ((c4_H & 0x20) >> 0x04) | ((c6_H & 0x20) >> 0x03) | ((c8_H & 0x20) >> 0x02);
    dst[0x1B] = ((c2_H & 0x10) >> 0x04) | ((c4_H & 0x10) >> 0x03) | ((c6_H & 0x10) >> 0x02) | ((c8_H & 0x10) >> 0x01);
    dst[0x1C] = ((c2_H & 0x08) >> 0x03) | ((c4_H & 0x08) >> 0x02) | ((c6_H & 0x08) >> 0x01) | ((c8_H & 0x08) >> 0x00);
    dst[0x1D] = ((c2_H & 0x04) >> 0x02) | ((c4_H & 0x04) >> 0x01) | ((c6_H & 0x04) >> 0x00) | ((c8_H & 0x04) << 0x01);
    dst[0x1E] = ((c2_H & 0x02) >> 0x01) | ((c4_H & 0x02) >> 0x00) | ((c6_H & 0x02) << 0x01) | ((c8_H & 0x02) << 0x02);
    dst[0x1F] = ((c2_H & 0x01) >> 0x00) | ((c4_H & 0x01) << 0x01) | ((c6_H & 0x01) << 0x02) | ((c8_H & 0x01) << 0x03);
    dst[0x20] = ((c2_M & 0x80) >> 0x07) | ((c4_M & 0x80) >> 0x06) | ((c6_M & 0x80) >> 0x05) | ((c8_M & 0x80) >> 0x04);
    dst[0x21] = ((c2_M & 0x40) >> 0x06) | ((c4_M & 0x40) >> 0x05) | ((c6_M & 0x40) >> 0x04) | ((c8_M & 0x40) >> 0x03);
    dst[0x22] = ((c2_M & 0x20) >> 0x05) | ((c4_M & 0x20) >> 0x04) | ((c6_M & 0x20) >> 0x03) | ((c8_M & 0x20) >> 0x02);
    dst[0x23] = ((c2_M & 0x10) >> 0x04) | ((c4_M & 0x10) >> 0x03) | ((c6_M & 0x10) >> 0x02) | ((c8_M & 0x10) >> 0x01);
    dst[0x24] = ((c2_M & 0x08) >> 0x03) | ((c4_M & 0x08) >> 0x02) | ((c6_M & 0x08) >> 0x01) | ((c8_M & 0x08) >> 0x00);
    dst[0x25] = ((c2_M & 0x04) >> 0x02) | ((c4_M & 0x04) >> 0x01) | ((c6_M & 0x04) >> 0x00) | ((c8_M & 0x04) << 0x01);
    dst[0x26] = ((c2_M & 0x02) >> 0x01) | ((c4_M & 0x02) >> 0x00) | ((c6_M & 0x02) << 0x01) | ((c8_M & 0x02) << 0x02);
    dst[0x27] = ((c2_M & 0x01) >> 0x00) | ((c4_M & 0x01) << 0x01) | ((c6_M & 0x01) << 0x02) | ((c8_M & 0x01) << 0x03);
    dst[0x28] = ((c2_L & 0x80) >> 0x07) | ((c4_L & 0x80) >> 0x06) | ((c6_L & 0x80) >> 0x05) | ((c8_L & 0x80) >> 0x04);
    dst[0x29] = ((c2_L & 0x40) >> 0x06) | ((c4_L & 0x40) >> 0x05) | ((c6_L & 0x40) >> 0x04) | ((c8_L & 0x40) >> 0x03);
    dst[0x2A] = ((c2_L & 0x20) >> 0x05) | ((c4_L & 0x20) >> 0x04) | ((c6_L & 0x20) >> 0x03) | ((c8_L & 0x20) >> 0x02);
    dst[0x2B] = ((c2_L & 0x10) >> 0x04) | ((c4_L & 0x10) >> 0x03) | ((c6_L & 0x10) >> 0x02) | ((c8_L & 0x10) >> 0x01);
    dst[0x2C] = ((c2_L & 0x08) >> 0x03) | ((c4_L & 0x08) >> 0x02) | ((c6_L & 0x08) >> 0x01) | ((c8_L & 0x08) >> 0x00);
    dst[0x2D] = ((c2_L & 0x04) >> 0x02) | ((c4_L & 0x04) >> 0x01) | ((c6_L & 0x04) >> 0x00) | ((c8_L & 0x04) << 0x01);
    dst[0x2E] = ((c2_L & 0x02) >> 0x01) | ((c4_L & 0x02) >> 0x00) | ((c6_L & 0x02) << 0x01) | ((c8_L & 0x02) << 0x02);
    dst[0x2F] = ((c2_L & 0x01) >> 0x00) | ((c4_L & 0x01) << 0x01) | ((c6_L & 0x01) << 0x02) | ((c8_L & 0x01) << 0x03);
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
// Packet structure: [432 bytes PCM (9 samples)][2 bytes MIDI][48 bytes PCM (1 sample)] = 482 bytes/packet
void PloytecWriteOutputInterrupt(uint8_t* ringBuffer, const float* srcFrames, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize, uint32_t bytesPerFrame) {
    for (uint32_t i = 0; i < frameCount; i++) {
        uint32_t sampleOffset = (uint32_t)((sampleTime + i) % ringSize);
        
        // Each logical packet = 80 frames = 8 USB sub-packets of 10 frames each
        uint32_t logicalPacket = sampleOffset / 80;
        uint32_t frameInLogicalPacket = sampleOffset % 80;
        uint32_t usbSubPacket = frameInLogicalPacket / 10;
        uint32_t sampleInSubPacket = frameInLogicalPacket % 10;
        
        // Calculate byte address within the USB sub-packet
        uint32_t sampleByteOffset;
        if (sampleInSubPacket < 9) {
            // Samples 0-8: at beginning of USB sub-packet
            sampleByteOffset = sampleInSubPacket * bytesPerFrame;
        } else {
            // Sample 9: after MIDI bytes (432 + 2)
            sampleByteOffset = 434;
        }
        
        uint32_t byteOffset = (logicalPacket * kOzzyMaxPacketSize) + (usbSubPacket * 482) + sampleByteOffset;
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
// Packet structure: [432 bytes PCM (9 samples)][2 bytes MIDI][48 bytes PCM (1 sample)] = 482 bytes/packet
void PloytecClearOutputInterrupt(uint8_t* outputBuffer, uint32_t bufferSize) {
    const uint32_t usbPacketSize = 482;
    const uint32_t pcm1Size = 432;
    const uint32_t pcm2Size = 48;
    const uint32_t numLogicalPackets = kOzzyNumPackets;  // 128 logical packets
    const uint32_t usbSubPacketsPerLogical = 8;
    
    for (uint32_t logicalPacket = 0; logicalPacket < numLogicalPackets; logicalPacket++) {
        uint32_t logicalPacketBase = logicalPacket * kOzzyMaxPacketSize;
        
        for (uint32_t subPacket = 0; subPacket < usbSubPacketsPerLogical; subPacket++) {
            uint8_t* usbPacket = outputBuffer + logicalPacketBase + (subPacket * usbPacketSize);
            memset(usbPacket, 0, pcm1Size);           // Clear PCM bytes 0-431 (9 samples)
            // Leave MIDI bytes 432-433 untouched for CoreMIDI
            memset(usbPacket + 434, 0, pcm2Size);     // Clear PCM bytes 434-481 (1 sample)
        }
    }
}
