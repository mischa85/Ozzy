/* SPDX-License-Identifier: MIT */
/*
 * USB MIDI 1.0 Event Packets
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "usb_midi.h"

/* MIDI bytes carried per CIN (USB MIDI 1.0, table 4-1); 0 = reserved */
static const uint8_t cin_length[16] = {
	0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1
};

/* Message length including status for 0x8n-0xEn, indexed by the high nibble */
static const uint8_t channel_msg_length[16] = {
	0, 0, 0, 0, 0, 0, 0, 0, 3, 3, 3, 3, 2, 2, 3, 0
};

static void put_packet(uint8_t packet[USB_MIDI_PACKET_SIZE], uint8_t cable, uint8_t cin,
		       uint8_t b0, uint8_t b1, uint8_t b2)
{
	packet[0] = (uint8_t)(cable << 4) | cin;
	packet[1] = b0;
	packet[2] = b1;
	packet[3] = b2;
}

/* Packet for a complete non-SysEx message in enc->msg */
static void put_message(const struct usb_midi_encoder *enc, uint8_t cable,
			uint8_t packet[USB_MIDI_PACKET_SIZE])
{
	uint8_t status = enc->msg[0];
	uint8_t cin;

	if (status < 0xF0)
		cin = status >> 4;
	else if (enc->len == 1)
		cin = USB_MIDI_CIN_SYSEX_END_1;    /* F6 */
	else if (enc->len == 2)
		cin = USB_MIDI_CIN_SYSCOM_2;       /* F1, F3 */
	else
		cin = USB_MIDI_CIN_SYSCOM_3;       /* F2 */

	put_packet(packet, cable, cin, enc->msg[0],
		   enc->len > 1 ? enc->msg[1] : 0, enc->len > 2 ? enc->msg[2] : 0);
}

bool usb_midi_encode_byte(struct usb_midi_encoder *enc, uint8_t cable, uint8_t byte,
			  uint8_t packet[USB_MIDI_PACKET_SIZE])
{
	/* Real-time messages may appear anywhere, even inside SysEx, and change no state */
	if (byte >= 0xF8) {
		put_packet(packet, cable, USB_MIDI_CIN_SINGLE_BYTE, byte, 0, 0);
		return true;
	}

	if (byte == 0xF7) {
		if (!enc->sysex)
			return false;
		enc->msg[enc->len++] = byte;
		put_packet(packet, cable, USB_MIDI_CIN_SYSEX_END_1 + enc->len - 1, enc->msg[0],
			   enc->len > 1 ? enc->msg[1] : 0, enc->len > 2 ? enc->msg[2] : 0);
		enc->sysex = false;
		enc->len = 0;
		return true;
	}

	if (byte >= 0x80) {
		/* Any other status byte ends SysEx (the F7 got lost) and running status */
		enc->sysex = false;
		enc->status = 0;
		enc->len = 0;
		enc->need = 0;

		if (byte == 0xF0) {
			enc->sysex = true;
			enc->msg[enc->len++] = byte;
			return false;
		}
		if (byte < 0xF0) {
			enc->status = byte;
			enc->need = channel_msg_length[byte >> 4];
		} else if (byte == 0xF2) {
			enc->need = 3;
		} else if (byte == 0xF1 || byte == 0xF3) {
			enc->need = 2;
		} else if (byte == 0xF6) {
			enc->need = 1;
		} else {
			return false;               /* F4, F5: undefined */
		}
		enc->msg[enc->len++] = byte;
		if (enc->len < enc->need)
			return false;
		put_message(enc, cable, packet);
		enc->len = 0;
		enc->need = 0;
		return true;
	}

	/* Data byte */
	if (enc->sysex) {
		enc->msg[enc->len++] = byte;
		if (enc->len < 3)
			return false;
		put_packet(packet, cable, USB_MIDI_CIN_SYSEX_START, enc->msg[0], enc->msg[1], enc->msg[2]);
		enc->len = 0;
		return true;
	}

	if (enc->len == 0) {
		if (!enc->status)
			return false;               /* no status to attach it to */
		enc->msg[enc->len++] = enc->status;  /* running status */
		enc->need = channel_msg_length[enc->status >> 4];
	}
	enc->msg[enc->len++] = byte;
	if (enc->len < enc->need)
		return false;
	put_message(enc, cable, packet);
	enc->len = 0;
	return true;
}

unsigned int usb_midi_decode_packet(const uint8_t packet[USB_MIDI_PACKET_SIZE], uint8_t out[3])
{
	unsigned int len = cin_length[packet[0] & 0x0F];

	out[0] = packet[1];
	out[1] = packet[2];
	out[2] = packet[3];
	return len;
}
