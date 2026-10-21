// SPDX-License-Identifier: MIT
/*
 * The control backend for macOS: OzzyHAL's devices through CoreAudio. Their UIDs start with
 * "OzzyHAL."; the settings are the custom property OZZY_PROPERTY_SETTING, a dictionary of
 * setting name -> value, the driver's record OZZY_PROPERTY_INFO a CFData of struct ozzy_info
 * (engine/shared/ozzy_shared_data.h); the rest (buffer size, latency, whether a client streams)
 * is CoreAudio's own. Changes come from property listeners: on the device list, and on each
 * Ozzy device's OZZY_PROPERTY_SETTING and OZZY_PROPERTY_INFO (OzzyHAL notifies them on every
 * set and after every handshake), rate, buffer size, use and liveness.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ozzy_control.h"

/* The macOS 12 SDK renamed kAudioObjectPropertyElementMaster; older SDKs only have that name */
#ifndef __MAC_12_0
#define kAudioObjectPropertyElementMain kAudioObjectPropertyElementMaster
#endif

/* Same values as engine/shared/ozzy_shared_data.h (its client side is C++, for the HAL) */
#define OZZY_PROPERTY_SETTING 'ozst'
#define OZZY_PROPERTY_INFO 'ozin'
#define OZZY_PROPERTY_STATS 'ozss'
#define OZZY_PROPERTY_CLIENTS 'ozcl'

/* libproc.h's, declared here: the header brings sys/socket.h's listen() */
int proc_name(int pid, void *buffer, uint32_t buffersize);
/* kAudioDevicePropertyZeroTimeStampPeriod, which only AudioServerPlugIn.h names */
#define ZERO_TIMESTAMP_PERIOD 'ring'

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
static void read_settings(AudioObjectID dev, struct ozzy_control_device *out)
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

static UInt32 get_u32(AudioObjectID obj, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope)
{
	AudioObjectPropertyAddress a = addr(sel, scope);
	UInt32 v = 0, sz = sizeof(v);

	if (AudioObjectGetPropertyData(obj, &a, 0, NULL, &sz, &v) != noErr)
		return 0;
	return v;
}

/* The CFData of custom property @selector, or NULL (the caller releases it) */
static CFDataRef copy_data(AudioObjectID dev, AudioObjectPropertySelector selector)
{
	AudioObjectPropertyAddress a = addr(selector, kAudioObjectPropertyScopeGlobal);
	CFDataRef data = NULL;
	UInt32 sz = sizeof(data);

	if (!AudioObjectHasProperty(dev, &a) ||
	    AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, &data) != noErr || !data)
		return NULL;
	if (CFGetTypeID(data) != CFDataGetTypeID()) {
		CFRelease(data);
		return NULL;
	}
	return data;
}

/* The driver's record (OZZY_PROPERTY_INFO) and stats (OZZY_PROPERTY_STATS), if it has ones this build reads */
static void read_info(AudioObjectID dev, struct ozzy_control_device *out)
{
	CFDataRef data = copy_data(dev, OZZY_PROPERTY_INFO);

	if (data) {
		out->has_info = ozzy_control_take_info(&out->info, CFDataGetBytePtr(data), (size_t)CFDataGetLength(data));
		CFRelease(data);
	}
	data = copy_data(dev, OZZY_PROPERTY_STATS);
	if (data) {
		out->has_stats = ozzy_control_take_stats(&out->stats, CFDataGetBytePtr(data), (size_t)CFDataGetLength(data));
		CFRelease(data);
	}
}

/* What CoreAudio has of the device */
/*
 * Whether process @pid does IO on @dev in that direction, from CoreAudio's process objects
 * (macOS 14 and later): 1 yes, 0 no, -1 if CoreAudio doesn't say
 */
static int process_uses(AudioObjectID dev, pid_t pid, bool output)
{
	AudioObjectPropertyAddress a = addr(kAudioHardwarePropertyTranslatePIDToProcessObject, kAudioObjectPropertyScopeGlobal);
	AudioObjectID proc = kAudioObjectUnknown, devs[16];
	UInt32 sz = sizeof(proc), i, n;

	if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof(pid), &pid, &sz, &proc) != noErr ||
	    proc == kAudioObjectUnknown)
		return -1;
	if (!get_u32(proc, output ? kAudioProcessPropertyIsRunningOutput : kAudioProcessPropertyIsRunningInput,
		     kAudioObjectPropertyScopeGlobal))
		return 0;
	a = addr(kAudioProcessPropertyDevices, output ? kAudioObjectPropertyScopeOutput : kAudioObjectPropertyScopeInput);
	sz = sizeof(devs);
	if (AudioObjectGetPropertyData(proc, &a, 0, NULL, &sz, devs) != noErr)
		return -1;
	n = sz / sizeof(devs[0]);
	for (i = 0; i < n; i++)
		if (devs[i] == dev)
			return 1;
	return 0;
}

/* ", " and @name (pid @pid) onto @list */
static void add_client(char *list, size_t size, unsigned int *count, const char *name, pid_t pid)
{
	size_t len = strlen(list);

	snprintf(list + len, size - len, "%s%s (pid %d)", len ? ", " : "", name, (int)pid);
	(*count)++;
}

/* How CoreAudio serves the clients in @list: it mixes them, unless one has the device to itself */
static void finish_clients(char *list, size_t size, unsigned int count, bool hog)
{
	size_t len = strlen(list);

	if (count)
		snprintf(list + len, size - len, "%s", hog ? ", exclusive (hog mode)" :
			 count > 1 ? ", mixed by CoreAudio" : ", CoreAudio");
}

/* The clients doing IO (OzzyHAL's OZZY_PROPERTY_CLIENTS), each way when CoreAudio says which */
static void read_clients(AudioObjectID dev, struct ozzy_control_host *h)
{
	AudioObjectPropertyAddress a = addr(OZZY_PROPERTY_CLIENTS, kAudioObjectPropertyScopeGlobal);
	AudioObjectPropertyAddress ha = addr(kAudioDevicePropertyHogMode, kAudioObjectPropertyScopeGlobal);
	unsigned int count[2] = { 0, 0 }, undirected = 0;
	bool hog_dir[2] = { false, false }, hog_any = false;
	CFArrayRef clients = NULL;
	UInt32 sz = sizeof(clients);
	pid_t hog = -1;
	CFIndex i;

	if (!AudioObjectHasProperty(dev, &a) || AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, &clients) != noErr ||
	    !clients)
		return;
	sz = sizeof(hog);
	if (AudioObjectGetPropertyData(dev, &ha, 0, NULL, &sz, &hog) != noErr)
		hog = -1;
	for (i = 0; CFGetTypeID(clients) == CFArrayGetTypeID() && i < CFArrayGetCount(clients); i++) {
		CFDictionaryRef c = CFArrayGetValueAtIndex(clients, i);
		CFNumberRef number;
		CFStringRef bundle;
		char name[128] = "";
		SInt32 pid = -1;
		int dir, uses[2];

		if (CFGetTypeID(c) != CFDictionaryGetTypeID())
			continue;
		number = CFDictionaryGetValue(c, CFSTR("pid"));
		if (number)
			CFNumberGetValue(number, kCFNumberSInt32Type, &pid);
		if (pid <= 0 || proc_name(pid, name, sizeof(name)) <= 0) {
			bundle = CFDictionaryGetValue(c, CFSTR("bundle"));
			if (!bundle || !CFStringGetCString(bundle, name, sizeof(name), kCFStringEncodingUTF8))
				snprintf(name, sizeof(name), "a client");
		}
		uses[0] = pid > 0 ? process_uses(dev, pid, true) : -1;
		uses[1] = pid > 0 ? process_uses(dev, pid, false) : -1;
		if (uses[0] < 0 || uses[1] < 0) {
			add_client(h->clients, sizeof(h->clients), &undirected, name, pid);
			hog_any |= pid == hog;
			continue;
		}
		for (dir = 0; dir < 2; dir++) {
			if (!uses[dir])
				continue;
			add_client(h->client[dir], sizeof(h->client[dir]), &count[dir], name, pid);
			hog_dir[dir] |= pid == hog;
		}
	}
	CFRelease(clients);
	finish_clients(h->client[0], sizeof(h->client[0]), count[0], hog_dir[0]);
	finish_clients(h->client[1], sizeof(h->client[1]), count[1], hog_dir[1]);
	finish_clients(h->clients, sizeof(h->clients), undirected, hog_any);
}

static void read_host(AudioObjectID dev, struct ozzy_control_device *out)
{
	struct ozzy_control_host *h = &out->host;

	snprintf(h->driver, sizeof(h->driver), "Ozzy kext + OzzyHAL");
	h->running = get_u32(dev, kAudioDevicePropertyDeviceIsRunningSomewhere, kAudioObjectPropertyScopeGlobal) != 0;
	h->buffer_frames = get_u32(dev, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal);
	h->latency[0] = get_u32(dev, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput);
	h->latency[1] = get_u32(dev, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput);
	h->safety_offset[0] = get_u32(dev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput);
	h->safety_offset[1] = get_u32(dev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput);
	h->timestamp_period = get_u32(dev, ZERO_TIMESTAMP_PERIOD, kAudioObjectPropertyScopeGlobal);
	snprintf(h->format, sizeof(h->format), "32-bit float, 24-bit on the device");
	read_clients(dev, h);
}

int ozzy_control_backend_list(struct ozzy_control_device *devs, int max)
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
		struct ozzy_control_device *d = &devs[n];
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
		read_info(ids[i], d);
		read_host(ids[i], d);
		n++;
	}
	free(ids);
	return n;
}

int ozzy_control_backend_set(const struct ozzy_control_device *dev, unsigned int id, unsigned int value)
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

/*
 * The watch (ozzy_control_backend_watch). CoreAudio is never called with a lock held, so an add or
 * remove of a listener can't wait on a callback that waits on us: state_lock guards only the list
 * of watched devices (by object and UID, as an object ID may come back for another device), and
 * callback_lock only makes the callbacks one at a time.
 */
#define WATCH_MAX OZZY_CONTROL_MAX_DEVICES

struct watched {
	AudioObjectID id;
	char uid[160];
};

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t callback_lock = PTHREAD_MUTEX_INITIALIZER;
static void (*watch_changed)(void *ctx);
static void *watch_ctx;
static struct watched watched[WATCH_MAX];
static int nwatched;

/* What of a device the listeners hear about */
static const AudioObjectPropertySelector device_props[] = {
	OZZY_PROPERTY_SETTING,
	OZZY_PROPERTY_INFO,
	OZZY_PROPERTY_STATS,
	OZZY_PROPERTY_CLIENTS,
	kAudioDevicePropertyNominalSampleRate,
	kAudioDevicePropertyBufferFrameSize,
	kAudioDevicePropertyDeviceIsRunningSomewhere,
	kAudioDevicePropertyStreamConfiguration,
	kAudioDevicePropertyDeviceIsAlive,
	kAudioObjectPropertyName,
};
#define NUM_DEVICE_PROPS (sizeof(device_props) / sizeof(device_props[0]))

static OSStatus on_property(AudioObjectID obj, UInt32 count, const AudioObjectPropertyAddress *addrs, void *ctx);

static void listen(AudioObjectID id, bool on)
{
	unsigned int p;

	for (p = 0; p < NUM_DEVICE_PROPS; p++) {
		AudioObjectPropertyAddress pa = addr(device_props[p], kAudioObjectPropertyScopeWildcard);

		if (on)
			AudioObjectAddPropertyListener(id, &pa, on_property, NULL);
		else
			AudioObjectRemovePropertyListener(id, &pa, on_property, NULL);
	}
}

static bool same_device(const struct watched *a, const struct watched *b)
{
	return a->id == b->id && !strcmp(a->uid, b->uid);
}

/*
 * Listen to devices that came, forget those that left: the list from CoreAudio, then the
 * difference under state_lock, then the listeners outside it. Two scans at once only race on
 * who adds or drops a device's listeners, which state_lock decides; at worst a device that just
 * left keeps a listener, which goes with it.
 */
static void watch_devices(void)
{
	AudioObjectPropertyAddress a = addr(kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal);
	struct watched now[WATCH_MAX], add[WATCH_MAX], drop[WATCH_MAX];
	AudioObjectID ids[64];
	UInt32 sz = sizeof(ids);
	int count = 0, nnow = 0, nadd = 0, ndrop = 0, i, k;

	if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, ids) == noErr)
		count = (int)(sz / sizeof(AudioObjectID));
	for (i = 0; i < count && nnow < WATCH_MAX; i++) {
		if (!get_string(ids[i], kAudioDevicePropertyDeviceUID, now[nnow].uid, sizeof(now[nnow].uid)) ||
		    strncmp(now[nnow].uid, "OzzyHAL.", 8) != 0)
			continue;
		now[nnow++].id = ids[i];
	}

	pthread_mutex_lock(&state_lock);
	for (i = 0; i < nnow; i++) {
		for (k = 0; k < nwatched && !same_device(&watched[k], &now[i]); k++)
			;
		if (k == nwatched)
			add[nadd++] = now[i];
	}
	for (k = 0; k < nwatched; k++) {
		for (i = 0; i < nnow && !same_device(&watched[k], &now[i]); i++)
			;
		if (i == nnow)
			drop[ndrop++] = watched[k];
	}
	memcpy(watched, now, sizeof(now[0]) * (size_t)nnow);
	nwatched = nnow;
	pthread_mutex_unlock(&state_lock);

	/* A device that left takes its listeners along; removed in case its object stays around */
	for (k = 0; k < ndrop; k++)
		listen(drop[k].id, false);
	for (i = 0; i < nadd; i++)
		listen(add[i].id, true);
}

static OSStatus on_property(AudioObjectID obj, UInt32 count, const AudioObjectPropertyAddress *addrs, void *ctx)
{
	(void)count;
	(void)addrs;
	(void)ctx;
	if (obj == kAudioObjectSystemObject)
		watch_devices();
	pthread_mutex_lock(&callback_lock);
	watch_changed(watch_ctx);
	pthread_mutex_unlock(&callback_lock);
	return noErr;
}

int ozzy_control_backend_watch(void (*changed)(void *ctx), void *ctx)
{
	AudioObjectPropertyAddress loop = addr(kAudioHardwarePropertyRunLoop, kAudioObjectPropertyScopeGlobal);
	AudioObjectPropertyAddress devices = addr(kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal);
	CFRunLoopRef none = NULL;
	OSStatus st;

	watch_changed = changed;
	watch_ctx = ctx;
	/* Notifications on CoreAudio's own thread, not a run loop the process may not run */
	AudioObjectSetPropertyData(kAudioObjectSystemObject, &loop, 0, NULL, sizeof(none), &none);
	/* The list's listener first, then the devices: a device that comes meanwhile is in one of them */
	st = AudioObjectAddPropertyListener(kAudioObjectSystemObject, &devices, on_property, NULL);
	if (st != noErr) {
		fprintf(stderr, "CoreAudio: can't watch the devices (error %d)\n", (int)st);
		return -1;
	}
	watch_devices();
	return 0;
}
