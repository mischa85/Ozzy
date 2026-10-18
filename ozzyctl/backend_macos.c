// SPDX-License-Identifier: MIT
/*
 * ozzyctl backend for macOS: OzzyHAL's devices through CoreAudio. Their UIDs start with
 * "OzzyHAL."; the settings are the custom property OZZY_PROPERTY_SETTING, a dictionary of
 * setting name -> value (engine/shared/ozzy_shared_data.h). Changes come from property
 * listeners: on the device list, and on each Ozzy device's OZZY_PROPERTY_SETTING (OzzyHAL
 * notifies it on every set and after every handshake), rate and liveness.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ozzyctl.h"

/* Same value as engine/shared/ozzy_shared_data.h (its client side is C++, for the HAL) */
#define OZZY_PROPERTY_SETTING 'ozst'

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

/* The device's settings from OZZY_PROPERTY_SETTING */
static void read_settings(AudioObjectID dev, struct ozzyctl_device *out)
{
	AudioObjectPropertyAddress a = addr(OZZY_PROPERTY_SETTING, kAudioObjectPropertyScopeGlobal);
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
	AudioObjectPropertyAddress a = addr(OZZY_PROPERTY_SETTING, kAudioObjectPropertyScopeGlobal);
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

/* The watch (ozzyctl_backend_watch): the devices with listeners, under watch_lock, which also
 * makes the callbacks one at a time */
static pthread_mutex_t watch_lock = PTHREAD_MUTEX_INITIALIZER;
static void (*watch_changed)(void *ctx);
static void *watch_ctx;
static AudioObjectID watched[64];
static int nwatched;

/* What of a device the listeners hear about */
static const AudioObjectPropertySelector device_props[] = {
	OZZY_PROPERTY_SETTING,
	kAudioDevicePropertyNominalSampleRate,
	kAudioDevicePropertyStreamConfiguration,
	kAudioDevicePropertyDeviceIsAlive,
	kAudioObjectPropertyName,
};
#define NUM_DEVICE_PROPS (sizeof(device_props) / sizeof(device_props[0]))

static OSStatus on_property(AudioObjectID obj, UInt32 count, const AudioObjectPropertyAddress *addrs, void *ctx);

/* Listen to devices that came, forget those that left: called with watch_lock held */
static void watch_devices(void)
{
	AudioObjectPropertyAddress a = addr(kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal);
	AudioObjectID ids[64], keep[64];
	UInt32 sz = sizeof(ids);
	int count = 0, nkeep = 0, i, k;
	unsigned int p;
	char uid[160];

	if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, ids) == noErr)
		count = (int)(sz / sizeof(AudioObjectID));
	for (i = 0; i < count && nkeep < 64; i++) {
		if (!get_string(ids[i], kAudioDevicePropertyDeviceUID, uid, sizeof(uid)) ||
		    strncmp(uid, "OzzyHAL.", 8) != 0)
			continue;
		for (k = 0; k < nwatched && watched[k] != ids[i]; k++)
			;
		if (k == nwatched) {
			for (p = 0; p < NUM_DEVICE_PROPS; p++) {
				AudioObjectPropertyAddress pa = addr(device_props[p], kAudioObjectPropertyScopeWildcard);

				AudioObjectAddPropertyListener(ids[i], &pa, on_property, NULL);
			}
		}
		keep[nkeep++] = ids[i];
	}
	/* A device that left takes its listeners along; removing them is for one that stays */
	for (k = 0; k < nwatched; k++) {
		for (i = 0; i < nkeep && keep[i] != watched[k]; i++)
			;
		for (p = 0; i == nkeep && p < NUM_DEVICE_PROPS; p++) {
			AudioObjectPropertyAddress pa = addr(device_props[p], kAudioObjectPropertyScopeWildcard);

			AudioObjectRemovePropertyListener(watched[k], &pa, on_property, NULL);
		}
	}
	memcpy(watched, keep, sizeof(keep[0]) * (size_t)nkeep);
	nwatched = nkeep;
}

static OSStatus on_property(AudioObjectID obj, UInt32 count, const AudioObjectPropertyAddress *addrs, void *ctx)
{
	(void)count;
	(void)addrs;
	(void)ctx;
	pthread_mutex_lock(&watch_lock);
	if (obj == kAudioObjectSystemObject)
		watch_devices();
	watch_changed(watch_ctx);
	pthread_mutex_unlock(&watch_lock);
	return noErr;
}

int ozzyctl_backend_watch(void (*changed)(void *ctx), void *ctx)
{
	AudioObjectPropertyAddress loop = addr(kAudioHardwarePropertyRunLoop, kAudioObjectPropertyScopeGlobal);
	AudioObjectPropertyAddress devices = addr(kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal);
	CFRunLoopRef none = NULL;
	OSStatus st;

	pthread_mutex_lock(&watch_lock);
	watch_changed = changed;
	watch_ctx = ctx;
	/* Notifications on CoreAudio's own thread, not a run loop the process may not run */
	AudioObjectSetPropertyData(kAudioObjectSystemObject, &loop, 0, NULL, sizeof(none), &none);
	st = AudioObjectAddPropertyListener(kAudioObjectSystemObject, &devices, on_property, NULL);
	if (st == noErr)
		watch_devices();
	pthread_mutex_unlock(&watch_lock);
	if (st != noErr) {
		fprintf(stderr, "CoreAudio: can't watch the devices (error %d)\n", (int)st);
		return -1;
	}
	return 0;
}
