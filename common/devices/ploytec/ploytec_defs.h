/* SPDX-License-Identifier: MIT */
/*
 * Ploytec USB Protocol Definitions
 *
 * Shared constants for Allen & Heath Xone series devices using the
 * Ploytec USB audio chipset. Used by both Linux and macOS drivers.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_PLOYTEC_DEFS_H
#define OZZY_PLOYTEC_DEFS_H

#ifdef __cplusplus
extern "C" {
#endif

/* USB vendor and product IDs */
#define PLOYTEC_VENDOR_ID           0x0A4A
#define PLOYTEC_PID_XONE_DB4       0xFFDB
#define PLOYTEC_PID_XONE_DB2       0xFFD2
#define PLOYTEC_PID_XONE_DX        0xFFDD
#define PLOYTEC_PID_XONE_4D        0xFF4D
#define PLOYTEC_PID_WIZARD_4       0xFFAD

/* USB endpoint addresses (low nibble) */
#define PLOYTEC_EP_PCM_OUT          0x05
#define PLOYTEC_EP_PCM_IN           0x06
#define PLOYTEC_EP_MIDI_IN          0x03

/* PCM topology */
#define PLOYTEC_CHANNELS            8
#define PLOYTEC_OUT_FRAME_SIZE      48  /* bytes per device output frame */
#define PLOYTEC_IN_FRAME_SIZE       64  /* bytes per device input frame */
#define PLOYTEC_FRAMES_PER_PKT      80  /* audio frames per USB packet (in and out) */

/* USB sub-packet sizes (minimal transfer unit) */
#define PLOYTEC_BULK_OUT_SUBPKT_SIZE  512   /* bytes per bulk output sub-packet */
#define PLOYTEC_INT_OUT_SUBPKT_SIZE   512   /* bytes per interrupt output sub-packet (same as bulk: 480 PCM + 2 MIDI + 30 pad) */
#define PLOYTEC_IN_SUBPKT_SIZE        512   /* bytes per input sub-packet (bulk & interrupt) */
#define PLOYTEC_FRAMES_PER_OUT_SUBPKT 10    /* audio frames per output sub-packet */
#define PLOYTEC_FRAMES_PER_IN_SUBPKT  8     /* audio frames per input sub-packet */

/* Packet sizes */
#define PLOYTEC_BULK_OUT_PKT_SIZE   4096    /* 8 bulk sub-packets (512 * 8) */
#define PLOYTEC_INT_OUT_PKT_SIZE    4096    /* 8 interrupt sub-packets (512 * 8, same bulk wire format confirmed by USB capture) */
#define PLOYTEC_IN_PKT_SIZE         5120    /* 10 input sub-packets (512 * 10) */

/* USB interface configuration */
#define PLOYTEC_NUM_INTERFACES      2
#define PLOYTEC_ALT_SETTING         1

/* Vendor request command bytes */
#define PLOYTEC_CMD_FIRMWARE        0x56  /* 'V' - read firmware version (15 bytes) */
#define PLOYTEC_CMD_STATUS          0x49  /* 'I' - read/write hardware status */
#define PLOYTEC_CMD_SET_RATE_REQ    0x01  /* bRequest for SET_CUR sample rate */
#define PLOYTEC_CMD_SET_RATE_TYPE   0x22  /* bmRequestType for SET_CUR sample rate */
#define PLOYTEC_CMD_GET_RATE_REQ    0x81  /* bRequest for GET_CUR sample rate */
#define PLOYTEC_CMD_GET_RATE_TYPE   0xA2  /* bmRequestType for GET_CUR sample rate */

/* SET_CUR sample rate endpoint addresses (wIndex field) */
#define PLOYTEC_EP_RATE_IN          0x0086
#define PLOYTEC_EP_RATE_OUT         0x0005

/* MIDI idle/sync byte (sent when no MIDI data is pending) */
#define PLOYTEC_MIDI_IDLE_BYTE      0xFD

#ifdef __cplusplus
}
#endif

#endif /* OZZY_PLOYTEC_DEFS_H */
