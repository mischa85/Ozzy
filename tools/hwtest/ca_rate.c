// SPDX-License-Identifier: MIT
/*
 * ca_rate: the sample rates a CoreAudio device offers, and setting one, as Audio MIDI Setup does.
 *
 *   ./ca_rate DB4           the rates and the one it runs at
 *   ./ca_rate DB4 44100     set it, wait until the device runs at it (4 s at most)
 *
 * Build: cc -O2 -o ca_rate ca_rate.c -framework CoreAudio -framework CoreFoundation
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static AudioObjectPropertyAddress addr(AudioObjectPropertySelector sel)
{
	AudioObjectPropertyAddress a = { sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
	return a;
}

static Float64 nominal_rate(AudioObjectID dev)
{
	AudioObjectPropertyAddress a = addr(kAudioDevicePropertyNominalSampleRate);
	Float64 rate = 0;
	UInt32 size = sizeof(rate);

	AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &rate);
	return rate;
}

/* The first device whose name contains @part, or 0 */
static AudioObjectID find_device(const char *part, char *name, size_t name_size)
{
	AudioObjectPropertyAddress a = addr(kAudioHardwarePropertyDevices);
	AudioObjectID ids[64];
	UInt32 size = sizeof(ids), i;

	if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, ids))
		return 0;
	for (i = 0; i < size / sizeof(ids[0]); i++) {
		AudioObjectPropertyAddress na = addr(kAudioObjectPropertyName);
		CFStringRef cf = NULL;
		UInt32 cf_size = sizeof(cf);

		if (AudioObjectGetPropertyData(ids[i], &na, 0, NULL, &cf_size, &cf) || !cf)
			continue;
		CFStringGetCString(cf, name, (CFIndex)name_size, kCFStringEncodingUTF8);
		CFRelease(cf);
		if (strstr(name, part))
			return ids[i];
	}
	return 0;
}

int main(int argc, char **argv)
{
	AudioObjectPropertyAddress a = addr(kAudioDevicePropertyAvailableNominalSampleRates);
	AudioValueRange ranges[16];
	UInt32 size = sizeof(ranges), i;
	AudioObjectID dev;
	char name[256];

	if (argc < 2) {
		fprintf(stderr, "usage: ca_rate NAME-PART [RATE]\n");
		return 2;
	}
	dev = find_device(argv[1], name, sizeof(name));
	if (!dev) {
		fprintf(stderr, "ca_rate: no device named like \"%s\"\n", argv[1]);
		return 1;
	}
	printf("%s: rates", name);
	if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, ranges) == noErr)
		for (i = 0; i < size / sizeof(ranges[0]); i++)
			printf(" %.0f", ranges[i].mMinimum);
	printf(", runs at %.0f\n", nominal_rate(dev));
	if (argc > 2) {
		AudioObjectPropertyAddress na = addr(kAudioDevicePropertyNominalSampleRate);
		Float64 want = atof(argv[2]);
		OSStatus st = AudioObjectSetPropertyData(dev, &na, 0, NULL, sizeof(want), &want);

		for (i = 0; i < 40 && nominal_rate(dev) != want; i++)
			usleep(100000);
		printf("set %.0f: status %d, runs at %.0f\n", want, (int)st, nominal_rate(dev));
		return st == noErr && nominal_rate(dev) == want ? 0 : 1;
	}
	return 0;
}
