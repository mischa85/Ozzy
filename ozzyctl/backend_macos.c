// SPDX-License-Identifier: MIT
/*
 * ozzyctl backend for macOS: OzzyHAL's devices through CoreAudio. Their UIDs start with
 * "OzzyHAL."; the settings are the custom property kOzzyPropertySetting, a dictionary of
 * setting name -> value (engine/shared/OzzySharedData.h).
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ozzyctl.h"

/* Same value as engine/shared/OzzySharedData.h (that header is C++ for the HAL and kext) */
#define kOzzyPropertySetting 'ozst'

static AudioObjectPropertyAddress addr(AudioObjectPropertySelector sel, AudioObjectPropertyScope scope)
{
	AudioObjectPropertyAddress a = { sel, scope, kAudioObjectPropertyElementMain };
	return a;
}

static bool get_string(AudioObjectID obj, AudioObjectPropertySelector sel, char *out, size_t size)
{
	AudioObjectPropertyAddress a = addr(sel, kAudioObjectPropertyScopeGlobal);
	CFStringRef s = NULL;
	UInt32 sz = sizeof(s);
	bool ok;

	out[0] = 0;
	if (AudioObjectGetPropertyData(obj, &a, 0, NULL, &sz, &s) != noErr || !s)
		return false;
	ok = CFStringGetCString(s, out, (CFIndex)size, kCFStringEncodingUTF8);
	CFRelease(s);
	return ok;
}

/* Channels in @scope: the sum over the stream configuration's buffers */
static unsigned int channels(AudioObjectID dev, AudioObjectPropertyScope scope)
{
	AudioObjectPropertyAddress a = addr(kAudioDevicePropertyStreamConfiguration, scope);
	AudioBufferList *list;
	unsigned int n = 0, i;
	UInt32 sz = 0;

	if (AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &sz) != noErr || !sz)
		return 0;
	list = malloc(sz);
	if (list && AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, list) == noErr)
		for (i = 0; i < list->mNumberBuffers; i++)
			n += list->mBuffers[i].mNumberChannels;
	free(list);
	return n;
}

static bool dict_int(CFDictionaryRef d, CFStringRef key, SInt32 *out)
{
	CFTypeRef v = CFDictionaryGetValue(d, key);

	if (v && CFGetTypeID(v) == CFNumberGetTypeID())
		return CFNumberGetValue((CFNumberRef)v, kCFNumberSInt32Type, out);
	if (v && CFGetTypeID(v) == CFBooleanGetTypeID()) {
		*out = CFBooleanGetValue((CFBooleanRef)v);
		return true;
	}
	return false;
}

/* The device's settings from kOzzyPropertySetting */
static void read_settings(AudioObjectID dev, struct ozzyctl_device *out)
{
	AudioObjectPropertyAddress a = addr(kOzzyPropertySetting, kAudioObjectPropertyScopeGlobal);
	CFDictionaryRef dict = NULL;
	UInt32 sz = sizeof(dict);
	unsigned int id;
	SInt32 v;

	ozzy_settings_defaults(&out->settings);
	if (!AudioObjectHasProperty(dev, &a) ||
	    AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, &dict) != noErr || !dict)
		return;
	if (CFGetTypeID(dict) == CFDictionaryGetTypeID()) {
		for (id = 0; id < OZZY_NUM_SETTINGS; id++) {
			CFStringRef key = CFStringCreateWithCString(NULL, ozzy_setting_descs[id].name,
								    kCFStringEncodingUTF8);

			if (key && dict_int(dict, key, &v) && v >= 0) {
				out->available |= 1u << id;
				out->settings.value[id] = (uint8_t)v;
			}
			if (key)
				CFRelease(key);
		}
		if (dict_int(dict, CFSTR("Clock Locked"), &v)) {
			out->has_clock = true;
			out->clock.locked = v != 0;
			if (dict_int(dict, CFSTR("Lock Rate"), &v))
				out->clock.lock_rate = (uint32_t)v;
		}
	}
	CFRelease(dict);
}

int ozzyctl_backend_list(struct ozzyctl_device *devs, int max)
{
	AudioObjectPropertyAddress a = addr(kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal);
	AudioObjectID *ids;
	UInt32 sz = 0;
	int n = 0, i, count;

	if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &sz) != noErr) {
		fprintf(stderr, "CoreAudio: can't list the devices\n");
		return -1;
	}
	ids = malloc(sz ? sz : 1);
	if (!ids || AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, ids) != noErr) {
		free(ids);
		fprintf(stderr, "CoreAudio: can't list the devices\n");
		return -1;
	}
	count = (int)(sz / sizeof(AudioObjectID));
	for (i = 0; i < count && n < max; i++) {
		struct ozzyctl_device *d = &devs[n];
		AudioObjectPropertyAddress ra = addr(kAudioDevicePropertyNominalSampleRate,
						     kAudioObjectPropertyScopeGlobal);
		Float64 rate = 0;
		UInt32 rsz = sizeof(rate);

		memset(d, 0, sizeof(*d));
		if (!get_string(ids[i], kAudioDevicePropertyDeviceUID, d->id, sizeof(d->id)) ||
		    strncmp(d->id, "OzzyHAL.", 8) != 0)
			continue;
		get_string(ids[i], kAudioObjectPropertyName, d->name, sizeof(d->name));
		if (AudioObjectGetPropertyData(ids[i], &ra, 0, NULL, &rsz, &rate) == noErr)
			d->sample_rate = (uint32_t)rate;
		d->in_channels = channels(ids[i], kAudioObjectPropertyScopeInput);
		d->out_channels = channels(ids[i], kAudioObjectPropertyScopeOutput);
		d->handle = ids[i];
		read_settings(ids[i], d);
		n++;
	}
	free(ids);
	return n;
}

int ozzyctl_backend_set(const struct ozzyctl_device *dev, unsigned int id, unsigned int value)
{
	AudioObjectPropertyAddress a = addr(kOzzyPropertySetting, kAudioObjectPropertyScopeGlobal);
	CFStringRef key = CFStringCreateWithCString(NULL, ozzy_setting_descs[id].name, kCFStringEncodingUTF8);
	SInt32 v = (SInt32)value;
	CFNumberRef num = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
	CFDictionaryRef dict = NULL;
	OSStatus st = -1;

	if (key && num)
		dict = CFDictionaryCreate(NULL, (const void **)&key, (const void **)&num, 1,
					  &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (dict)
		st = AudioObjectSetPropertyData((AudioObjectID)dev->handle, &a, 0, NULL, sizeof(dict), &dict);
	if (dict)
		CFRelease(dict);
	if (num)
		CFRelease(num);
	if (key)
		CFRelease(key);
	if (st != noErr) {
		fprintf(stderr, "%s: setting %s failed (CoreAudio error %d)\n", dev->name,
			ozzy_setting_descs[id].name, (int)st);
		return -1;
	}
	return 0;
}
