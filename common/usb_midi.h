/* SPDX-License-Identifier: MIT */
/*
 * USB MIDI 1.0 Event Packets
 *
 * Converts between a MIDI 1.0 byte stream and the 4-byte event packets of the
 * USB MIDI class (USB Device Class Definition for MIDI Devices 1.0, section 4):
 *
 *   byte 0: cable number (high nibble) | code index number (CIN, low nibble)
 *   bytes 1-3: MIDI bytes, zero padded
 *
 * Used for class MIDI devices that need an Ozzy driver anyway, such as the
 * Xone:1D, whose 4-byte bulk endpoints macOS's class driver rejects.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_USB_MIDI_H
#define OZZY_USB_MIDI_H

#if defined(__KERNEL__) && defined(__linux__)
#include <linux/types.h>
#else
#include <stdint.h>
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define USB_MIDI_PACKET_SIZE 4

/* Code index numbers (USB MIDI 1.0, table 4-1) */
enum usb_midi_cin {
	USB_MIDI_CIN_MISC           = 0x0,  /* reserved */
	USB_MIDI_CIN_CABLE_EVENT    = 0x1,  /* reserved */
	USB_MIDI_CIN_SYSCOM_2       = 0x2,  /* 2-byte system common (F1, F3) */
	USB_MIDI_CIN_SYSCOM_3       = 0x3,  /* 3-byte system common (F2) */
	USB_MIDI_CIN_SYSEX_START    = 0x4,  /* SysEx starts or continues */
	USB_MIDI_CIN_SYSEX_END_1    = 0x5,  /* 1-byte system common, or SysEx ends with 1 byte */
	USB_MIDI_CIN_SYSEX_END_2    = 0x6,  /* SysEx ends with 2 bytes */
	USB_MIDI_CIN_SYSEX_END_3    = 0x7,  /* SysEx ends with 3 bytes */
	USB_MIDI_CIN_NOTE_OFF       = 0x8,
	USB_MIDI_CIN_NOTE_ON        = 0x9,
	USB_MIDI_CIN_POLY_PRESSURE  = 0xA,
	USB_MIDI_CIN_CONTROL_CHANGE = 0xB,
	USB_MIDI_CIN_PROGRAM_CHANGE = 0xC,
	USB_MIDI_CIN_CHAN_PRESSURE  = 0xD,
	USB_MIDI_CIN_PITCH_BEND     = 0xE,
	USB_MIDI_CIN_SINGLE_BYTE    = 0xF,  /* single byte (real-time messages) */
};

/*
 * struct usb_midi_encoder - Byte stream to event packet state.
 * Zero-initialise before use. Keeps running status, so the input may use it.
 */
struct usb_midi_encoder {
	uint8_t msg[3];     /* message being collected */
	uint8_t len;        /* bytes in msg */
	uint8_t need;       /* message length including status, 0 if no status */
	uint8_t status;     /* running status (channel messages only), 0 if none */
	bool sysex;         /* inside F0 ... F7 */
};

/*
 * usb_midi_encode_byte - Feed one MIDI byte.
 * Writes an event packet for cable @cable to @packet when @byte completes one.
 * Returns true if a packet was written.
 */
bool usb_midi_encode_byte(struct usb_midi_encoder *enc, uint8_t cable, uint8_t byte,
			  uint8_t packet[USB_MIDI_PACKET_SIZE]);

/*
 * usb_midi_decode_packet - MIDI bytes carried by one event packet.
 * Copies them to @out (room for 3) and returns how many there are; 0 for
 * padding and reserved CINs.
 */
unsigned int usb_midi_decode_packet(const uint8_t packet[USB_MIDI_PACKET_SIZE], uint8_t out[3]);

#ifdef __cplusplus
}
#endif

#endif /* OZZY_USB_MIDI_H */
