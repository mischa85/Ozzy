// SPDX-License-Identifier: MIT
/*
 * midi_listen: the MIDI a CoreMIDI source sends, one message per line in hex, for a number of
 * seconds; the endpoints first. On Linux, amidi -l and amidi -p hw:N,0 -d do the same.
 *
 *   ./midi_listen DB4 60
 *
 * Build: cc -O2 -o midi_listen midi_listen.c -framework CoreMIDI -framework CoreFoundation
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int messages;

static void endpoint_name(MIDIEndpointRef e, char *out, size_t size)
{
	CFStringRef cf = NULL;

	out[0] = 0;
	if (MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &cf) == noErr && cf) {
		CFStringGetCString(cf, out, (CFIndex)size, kCFStringEncodingUTF8);
		CFRelease(cf);
	}
}

static void on_packets(const MIDIPacketList *list, void *port_ref, void *src_ref)
{
	const MIDIPacket *p = &list->packet[0];
	UInt32 i, k;

	(void)port_ref;
	(void)src_ref;
	for (i = 0; i < list->numPackets; i++, p = MIDIPacketNext(p)) {
		for (k = 0; k < p->length; k++)
			printf("%s%02X", k ? " " : "", p->data[k]);
		printf("\n");
		messages++;
	}
	fflush(stdout);
}

int main(int argc, char **argv)
{
	const char *part = argc > 1 ? argv[1] : "";
	double seconds = argc > 2 ? atof(argv[2]) : 30;
	MIDIClientRef client;
	MIDIPortRef port;
	ItemCount i;
	char name[256];
	int connected = 0;

	for (i = 0; i < MIDIGetNumberOfSources(); i++) {
		endpoint_name(MIDIGetSource(i), name, sizeof(name));
		printf("source: %s\n", name);
	}
	for (i = 0; i < MIDIGetNumberOfDestinations(); i++) {
		endpoint_name(MIDIGetDestination(i), name, sizeof(name));
		printf("destination: %s\n", name);
	}
	MIDIClientCreate(CFSTR("midi_listen"), NULL, NULL, &client);
	MIDIInputPortCreate(client, CFSTR("in"), on_packets, NULL, &port);
	for (i = 0; i < MIDIGetNumberOfSources(); i++) {
		endpoint_name(MIDIGetSource(i), name, sizeof(name));
		if (strstr(name, part) && MIDIPortConnectSource(port, MIDIGetSource(i), NULL) == noErr)
			connected++;
	}
	if (!connected) {
		fprintf(stderr, "midi_listen: no source named like \"%s\"\n", part);
		return 1;
	}
	fflush(stdout);
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
	printf("%d messages\n", messages);
	return 0;
}
