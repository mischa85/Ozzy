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

#include "../../ozzy_ep.h"          /* PLOYTEC_ISOC_EP_xxx */
#include "../../isoc_feedback.h"    /* PLOYTEC_ISOC_FEEDBACK */

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
#define PLOYTEC_PID_XONE_2D        0xFF01
#define PLOYTEC_PID_XONE_1D        0x001E   /* MIDI only: USB MIDI class, not the Ploytec audio protocol */

/* Other vendors' Ploytec-based devices */
#define RELOOP_VENDOR_ID            0x200C
#define RELOOP_PID_DIGITAL_JOCKEY_2 0x1009   /* Digital Jockey 2 Master Edition */
#define AKAI_VENDOR_ID              0x09E8
#define AKAI_PID_EIE_PRO            0x0010   /* bulk in / isochronous out, see .claude/re/akai_eie_pro.md */

/*
 * Bulk in / isoc out devices (Akai EIE Pro), as both platforms describe them.
 * Endpoints are found by type like the official driver does (struct ozzy_ep); on the
 * EIE Pro: PCM out 0x02, MIDI out 0x04, MIDI in 0x83 on interface 0, PCM in 0x86 and
 * feedback 0x81 on interface 1.
 * Feedback (common/isoc_feedback.h): 3-byte packets, byte 0 = frames played in the
 * interval. More than 40 wrong-length packets without a clean ring lap stop the
 * official driver.
 */
#define PLOYTEC_ISOC_FB_PACKET_SIZE 3
#define PLOYTEC_ISOC_FB_MAX_ERRORS  40

#define PLOYTEC_ISOC_EP_PCM_OUT   { 0, 0, OZZY_EP_ISOC, OZZY_EP_DIR_OUT }
#define PLOYTEC_ISOC_EP_MIDI_OUT  { 0, 0, OZZY_EP_BULK | OZZY_EP_INT, OZZY_EP_DIR_OUT }
#define PLOYTEC_ISOC_EP_MIDI_IN   { 0, 0, OZZY_EP_BULK | OZZY_EP_INT, OZZY_EP_DIR_IN }
#define PLOYTEC_ISOC_EP_PCM_IN    { 1, 0, OZZY_EP_BULK | OZZY_EP_INT, OZZY_EP_DIR_IN }
#define PLOYTEC_ISOC_EP_FEEDBACK  { 1, 0, OZZY_EP_ISOC, OZZY_EP_DIR_IN }
#define PLOYTEC_ISOC_FEEDBACK     { ISOC_FB_COUNT, PLOYTEC_ISOC_FB_PACKET_SIZE, PLOYTEC_ISOC_FB_MAX_ERRORS }

/* USB endpoint addresses (low nibble) */
#define PLOYTEC_EP_PCM_OUT          0x05
#define PLOYTEC_EP_PCM_IN           0x06
#define PLOYTEC_EP_MIDI_IN          0x03

/* PCM topology */
#define PLOYTEC_MAX_OUT_CHANNELS    8   /* audio lanes in an output frame */
#define PLOYTEC_MAX_IN_CHANNELS     16  /* audio lanes in an input frame */
#define PLOYTEC_OUT_FRAME_SIZE      48  /* bytes per device output frame */
#define PLOYTEC_IN_FRAME_SIZE       64  /* bytes per device input frame */
#define PLOYTEC_FRAMES_PER_PKT      80  /* audio frames per USB packet (in and out) */

/* USB sub-packet sizes (minimal transfer unit) */
#define PLOYTEC_BULK_OUT_SUBPKT_SIZE  512   /* bytes per bulk output sub-packet */
#define PLOYTEC_INT_OUT_SUBPKT_SIZE   482   /* bytes per interrupt output sub-packet */
#define PLOYTEC_IN_SUBPKT_SIZE        512   /* bytes per input sub-packet (bulk & interrupt) */
#define PLOYTEC_FRAMES_PER_OUT_SUBPKT 10    /* audio frames per output sub-packet */
#define PLOYTEC_FRAMES_PER_IN_SUBPKT  8     /* audio frames per input sub-packet */

/* Packet sizes */
#define PLOYTEC_BULK_OUT_PKT_SIZE   4096    /* 8 bulk sub-packets (512 * 8) */
#define PLOYTEC_INT_OUT_PKT_SIZE    3856    /* 8 interrupt sub-packets (482 * 8) */
#define PLOYTEC_IN_PKT_SIZE         5120    /* 10 input sub-packets (512 * 10) */

/* USB interface configuration */
#define PLOYTEC_NUM_INTERFACES      2
#define PLOYTEC_ALT_SETTING         1

/* bmRequestType for vendor requests ('V', 'I') */
#define PLOYTEC_REQTYPE_VENDOR_IN   0xC0  /* device-to-host, vendor, device */
#define PLOYTEC_REQTYPE_VENDOR_OUT  0x40  /* host-to-device, vendor, device */

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
#define PLOYTEC_ISOC_EP_RATE_OUT    0x0002   /* bulk in / isoc out devices: their isoc PCM OUT EP */

/* MIDI idle/sync byte (sent when no MIDI data is pending) */
#define PLOYTEC_MIDI_IDLE_BYTE      0xFD

/* Control byte after each MIDI out slot when there is no control data (see ploytec_midi.h) */
#define PLOYTEC_MIDI_CONTROL_IDLE   0xFF

#ifdef __cplusplus
}
#endif

#endif /* OZZY_PLOYTEC_DEFS_H */
