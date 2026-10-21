// SPDX-License-Identifier: MIT
/*
 * The control backend for Windows: the Ozzy ACX driver's property set (windows/driver/ozzy_ksproperty.h)
 * through IOCTL_KS_PROPERTY (the status and settings, and the driver's record struct ozzy_info).
 * Each device has a KSCATEGORY_AUDIO interface per endpoint; the
 * first one that answers the property stands for the device (the driver serves it on both).
 * Devices with another driver (the official one) don't answer, and aren't listed. Changes come
 * from the driver's KS event on that filter (OZZY_KSEVENT_CHANGED) and CM notifications for the
 * interfaces coming and going, on one thread waiting for them.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <ks.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ozzy_control.h"
#include "../../common/ozzy_devices.h"

/* DEVPKEY_Device_BusReportedDeviceDesc (devpkey.h, here so no initguid.h is needed): the
 * device's own name, its USB product string, as Linux and macOS show it ("Allen&Heath
 * Xone:DB4"); the friendly name is the INF's model name ("Xone:DB4") */
static const DEVPROPKEY ozzy_bus_reported_desc = {
	{ 0x540b947e, 0x8b40, 0x45bc, { 0xa8, 0xa2, 0x6a, 0x0b, 0x89, 0x4c, 0xbd, 0xa2 } }, 4 };
#include "../../windows/driver/ozzy_ksproperty.h"

/* KSCATEGORY_AUDIO, here so no GUID library is needed */
static const GUID ksaudio = { 0x6994ad04, 0x93ef, 0x11d0, { 0xa3, 0xcc, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96 } };
static const GUID ozzy_propsetid_settings = OZZY_PROPSETID_SETTINGS_INIT;
static const GUID ozzy_eventsetid_settings = OZZY_EVENTSETID_SETTINGS_INIT;

/* Interface paths of the listed devices with their ids (a set finds its device by id: the list
 * may have changed since the caller's); a front end may list on one thread and set on another */
static wchar_t paths[OZZY_CONTROL_MAX_DEVICES][512];
static char path_ids[OZZY_CONTROL_MAX_DEVICES][160];
static int npaths;
static SRWLOCK paths_lock = SRWLOCK_INIT;

/* One property call on the filter at @path: get into @value, or set from it */
static bool ks_property(const wchar_t *path, ULONG id, ULONG flags, void *value, DWORD size, DWORD *returned)
{
	KSPROPERTY prop;
	HANDLE h;
	BOOL ok;

	/* Shared: the watch keeps the filter open */
	h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	memset(&prop, 0, sizeof(prop));
	prop.Set = ozzy_propsetid_settings;
	prop.Id = id;
	prop.Flags = flags;
	ok = DeviceIoControl(h, IOCTL_KS_PROPERTY, &prop, sizeof(prop), value, size, returned, NULL);
	CloseHandle(h);
	return ok != 0;
}

static void to_utf8(const wchar_t *w, char *out, int size)
{
	if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, size, NULL, NULL))
		out[0] = 0;
}

/* Our devices into @devs, each one's interface path into @found */
static int find_devices(struct ozzy_control_device *devs, int max, wchar_t (*found)[512])
{
	SP_DEVICE_INTERFACE_DATA ifd = { sizeof(ifd) };
	HDEVINFO set;
	DWORD i;
	int n = 0;

	set = SetupDiGetClassDevsW(&ksaudio, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (set == INVALID_HANDLE_VALUE) {
		fprintf(stderr, "can't list audio devices (error %lu)\n", GetLastError());
		return -1;
	}
	for (i = 0; n < max && n < OZZY_CONTROL_MAX_DEVICES && SetupDiEnumDeviceInterfaces(set, NULL, &ksaudio, i, &ifd); i++) {
		SP_DEVINFO_DATA dev = { sizeof(dev) };
		SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail;
		struct ozzy_control_device *d = &devs[n];
		struct ozzy_ksproperty_status st;
		const struct ozzy_device *model;
		wchar_t ids[1024] = { 0 }, instance[200], friendly[128] = { 0 };
		DWORD needed = 0, returned = 0;
		unsigned int id;
		bool dup = false;
		int k, dir;

		/* The device behind the interface: one of ours (by hardware ID), listed once */
		SetupDiGetDeviceInterfaceDetailW(set, &ifd, NULL, 0, &needed, NULL);
		detail = needed ? malloc(needed) : NULL;
		if (!detail)
			continue;
		detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
		if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, needed, NULL, &dev) ||
		    !SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, NULL, (BYTE *)ids,
						       sizeof(ids) - 2 * sizeof(wchar_t), NULL) ||
		    !(model = ozzy_device_lookup_hwid((const uint16_t *)ids)) ||
		    !SetupDiGetDeviceInstanceIdW(set, &dev, instance, 200, NULL)) {
			free(detail);
			continue;
		}
		memset(d, 0, sizeof(*d));
		to_utf8(instance, d->id, sizeof(d->id));
		d->id_internal = true;   /* the device instance path: no use to look at */
		for (k = 0; k < n; k++)
			dup |= !strcmp(devs[k].id, d->id);
		if (dup) {
			free(detail);
			continue;
		}

		/* The Ozzy driver answers its property; another driver doesn't */
		memset(&st, 0, sizeof(st));
		if (!ks_property(detail->DevicePath, OZZY_KSPROPERTY_STATUS, KSPROPERTY_TYPE_GET, &st, sizeof(st), &returned) ||
		    returned < sizeof(st) || st.version != OZZY_KSPROPERTY_VERSION) {
			free(detail);
			continue;
		}
		wcsncpy_s(found[n], 512, detail->DevicePath, _TRUNCATE);
		free(detail);

		/* The device's own name, else the friendly name, else the model's */
		{
			DEVPROPTYPE type = 0;

			memset(friendly, 0, sizeof(friendly));
			if (SetupDiGetDevicePropertyW(set, &dev, &ozzy_bus_reported_desc, &type, (BYTE *)friendly,
						      sizeof(friendly) - sizeof(wchar_t), NULL, 0) &&
			    type == DEVPROP_TYPE_STRING)
				to_utf8(friendly, d->name, sizeof(d->name));
		}
		if (!d->name[0] &&
		    SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_FRIENDLYNAME, NULL, (BYTE *)friendly,
						      sizeof(friendly) - sizeof(wchar_t), NULL))
			to_utf8(friendly, d->name, sizeof(d->name));
		if (!d->name[0])
			snprintf(d->name, sizeof(d->name), "%s", model->name);
		d->handle = (uintptr_t)n;
		d->sample_rate = st.sample_rate;
		d->in_channels = st.in_channels;
		d->out_channels = st.out_channels;
		d->available = st.available;
		ozzy_settings_defaults(&d->settings);
		for (id = 0; id < OZZY_NUM_SETTINGS; id++)
			if (st.available & (1u << id))
				d->settings.value[id] = st.value[id];
		d->has_clock = (st.available & (1u << OZZY_SETTING_CLOCK_SOURCE)) != 0;
		d->clock.locked = st.clock_locked != 0;
		d->host.running = st.running != 0;   /* a stream runs in either direction */
		/* The WaveRT stream (output's if there is one): its packet is the audio engine's period */
		dir = st.packet_frames[0] ? 0 : 1;
		if (st.packet_frames[dir]) {
			snprintf(d->host.format, sizeof(d->host.format), "S24_3LE");
			d->host.buffer_frames = st.packet_frames[dir];
			d->host.ring_frames = st.packet_frames[dir] * st.packets[dir];
		}
		d->host.latency[0] = st.fifo_frames[0];
		d->host.latency[1] = st.fifo_frames[1];
		d->clock.lock_rate = st.clock_locked ? st.lock_rate : 0;
		snprintf(d->host.driver, sizeof(d->host.driver), "Ozzy (ACX)");
		{
			struct ozzy_info info;

			memset(&info, 0, sizeof(info));
			if (ks_property(found[n], OZZY_KSPROPERTY_INFO, KSPROPERTY_TYPE_GET, &info, sizeof(info), &returned))
				d->has_info = ozzy_control_take_info(&d->info, &info, returned);
		}
		{
			struct ozzy_stats stats;

			memset(&stats, 0, sizeof(stats));
			if (ks_property(found[n], OZZY_KSPROPERTY_STATS, KSPROPERTY_TYPE_GET, &stats, sizeof(stats), &returned))
				d->has_stats = ozzy_control_take_stats(&d->stats, &stats, returned);
		}
		n++;
	}
	SetupDiDestroyDeviceInfoList(set);
	return n;
}

int ozzy_control_backend_list(struct ozzy_control_device *devs, int max)
{
	static wchar_t found[OZZY_CONTROL_MAX_DEVICES][512];
	int n;

	AcquireSRWLockExclusive(&paths_lock);
	n = find_devices(devs, max, found);
	npaths = n > 0 ? n : 0;
	for (int i = 0; i < npaths; i++) {
		memcpy(paths[i], found[i], sizeof(found[i]));
		memcpy(path_ids[i], devs[i].id, sizeof(path_ids[i]));
	}
	ReleaseSRWLockExclusive(&paths_lock);
	return n;
}

int ozzy_control_backend_set(const struct ozzy_control_device *dev, unsigned int id, unsigned int value)
{
	struct ozzy_ksproperty_setting s = { id, value };
	wchar_t path[512];
	DWORD returned = 0;

	int i;

	AcquireSRWLockShared(&paths_lock);
	for (i = 0; i < npaths && strcmp(path_ids[i], dev->id); i++)
		;
	if (i < npaths)
		wcsncpy_s(path, 512, paths[i], _TRUNCATE);
	ReleaseSRWLockShared(&paths_lock);
	if (i == npaths) {
		fprintf(stderr, "%s: not connected any more\n", dev->name);
		return -1;
	}
	/* A device setting restarts the device inside this call (streams glitch) */
	if (!ks_property(path, OZZY_KSPROPERTY_SETTING, KSPROPERTY_TYPE_SET, &s, sizeof(s), &returned)) {
		fprintf(stderr, "%s: setting %s failed (error %lu)\n", dev->name, ozzy_setting_descs[id].name,
			GetLastError());
		return -1;
	}
	return 0;
}

/*
 * The watch (ozzy_control_backend_watch). Each device's filter stays open with the event enabled; an
 * open handle holds up the device's removal, so it is closed as soon as removal is asked for.
 */
struct watch_sub {
	HANDLE volatile file;          /* INVALID_HANDLE_VALUE once closed */
	HANDLE event;                  /* the driver signals it */
	HCMNOTIFICATION removal;       /* NULL if none */
};

static struct {
	void (*changed)(void *ctx);
	void *ctx;
	HANDLE rescan;                 /* auto-reset: devices came or go */
	HCMNOTIFICATION interfaces;
	struct watch_sub subs[OZZY_CONTROL_MAX_DEVICES];
	int nsubs;
} watch;

static void sub_close_file(struct watch_sub *sub)
{
	HANDLE h = InterlockedExchangePointer((PVOID volatile *)&sub->file, INVALID_HANDLE_VALUE);

	if (h != INVALID_HANDLE_VALUE)
		CloseHandle(h);
}

/* An audio interface came or went */
static DWORD CALLBACK on_interface(HCMNOTIFICATION note, PVOID ctx, CM_NOTIFY_ACTION action,
				   PCM_NOTIFY_EVENT_DATA data, DWORD size)
{
	(void)note;
	(void)ctx;
	(void)action;
	(void)data;
	(void)size;
	SetEvent(watch.rescan);
	return ERROR_SUCCESS;
}

/* A watched device is to be removed: let go of it, and look again once that is decided (not
 * before: opening it again would hold the removal up). Removal refused: it is opened again. */
static DWORD CALLBACK on_device(HCMNOTIFICATION note, PVOID ctx, CM_NOTIFY_ACTION action,
				PCM_NOTIFY_EVENT_DATA data, DWORD size)
{
	struct watch_sub *sub = ctx;

	(void)note;
	(void)data;
	(void)size;
	switch (action) {
	case CM_NOTIFY_ACTION_DEVICEQUERYREMOVE:
	case CM_NOTIFY_ACTION_DEVICEREMOVEPENDING:
		sub_close_file(sub);
		break;
	case CM_NOTIFY_ACTION_DEVICEREMOVECOMPLETE:
		sub_close_file(sub);
		SetEvent(watch.rescan);
		break;
	case CM_NOTIFY_ACTION_DEVICEQUERYREMOVEFAILED:
		SetEvent(watch.rescan);
		break;
	default:
		break;
	}
	return ERROR_SUCCESS;
}

/* Called on the watch thread only (CM_Unregister_Notification must not run in a callback) */
static void watch_close(void)
{
	int i;

	for (i = 0; i < watch.nsubs; i++) {
		if (watch.subs[i].removal)
			CM_Unregister_Notification(watch.subs[i].removal);
		sub_close_file(&watch.subs[i]);
		CloseHandle(watch.subs[i].event);
	}
	watch.nsubs = 0;
}

static void watch_open(void)
{
	struct ozzy_control_device devs[OZZY_CONTROL_MAX_DEVICES];
	static wchar_t found[OZZY_CONTROL_MAX_DEVICES][512];
	int n = find_devices(devs, OZZY_CONTROL_MAX_DEVICES, found), i;

	for (i = 0; i < n; i++) {
		struct watch_sub *sub = &watch.subs[watch.nsubs];
		CM_NOTIFY_FILTER filter;
		KSEVENTDATA data;
		KSEVENT ev;
		DWORD returned = 0;
		HANDLE h, event;

		h = CreateFileW(found[i], GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
				OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (h == INVALID_HANDLE_VALUE)
			continue;
		event = CreateEventW(NULL, FALSE, FALSE, NULL);
		memset(&ev, 0, sizeof(ev));
		ev.Set = ozzy_eventsetid_settings;
		ev.Id = OZZY_KSEVENT_CHANGED;
		ev.Flags = KSEVENT_TYPE_ENABLE;
		memset(&data, 0, sizeof(data));
		data.NotificationType = KSEVENTF_EVENT_HANDLE;
		data.EventHandle.Event = event;
		/* The event goes with the handle: closing it disables the event */
		if (!event || !DeviceIoControl(h, IOCTL_KS_ENABLE_EVENT, &ev, sizeof(ev), &data, sizeof(data), &returned, NULL)) {
			if (event)
				CloseHandle(event);
			CloseHandle(h);
			continue;
		}
		sub->file = h;
		sub->event = event;
		sub->removal = NULL;
		memset(&filter, 0, sizeof(filter));
		filter.cbSize = sizeof(filter);
		filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEHANDLE;
		filter.u.DeviceHandle.hTarget = h;
		if (CM_Register_Notification(&filter, sub, on_device, &sub->removal) != CR_SUCCESS)
			sub->removal = NULL;
		watch.nsubs++;
	}
}

static DWORD WINAPI watch_thread(LPVOID arg)
{
	HANDLE waits[1 + OZZY_CONTROL_MAX_DEVICES];

	(void)arg;
	for (;;) {
		DWORD r;
		int i;

		waits[0] = watch.rescan;
		for (i = 0; i < watch.nsubs; i++)
			waits[1 + i] = watch.subs[i].event;
		r = WaitForMultipleObjects((DWORD)(1 + watch.nsubs), waits, FALSE, INFINITE);
		if (r == WAIT_FAILED) {
			fprintf(stderr, "watch: waiting failed (error %lu)\n", GetLastError());
			return 1;
		}
		if (r == WAIT_OBJECT_0) {
			watch_close();
			watch_open();
		}
		watch.changed(watch.ctx);
	}
}

int ozzy_control_backend_watch(void (*changed)(void *ctx), void *ctx)
{
	CM_NOTIFY_FILTER filter;
	HANDLE thread;

	watch.changed = changed;
	watch.ctx = ctx;
	watch.rescan = CreateEventW(NULL, FALSE, FALSE, NULL);
	memset(&filter, 0, sizeof(filter));
	filter.cbSize = sizeof(filter);
	filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
	filter.u.DeviceInterface.ClassGuid = ksaudio;
	if (!watch.rescan || CM_Register_Notification(&filter, NULL, on_interface, &watch.interfaces) != CR_SUCCESS) {
		fprintf(stderr, "can't watch audio devices (error %lu)\n", GetLastError());
		return -1;
	}
	/* Subscribed before returning: a change right after is heard */
	watch_open();
	thread = CreateThread(NULL, 0, watch_thread, NULL, 0, NULL);
	if (!thread) {
		fprintf(stderr, "can't start the watch thread (error %lu)\n", GetLastError());
		return -1;
	}
	CloseHandle(thread);
	return 0;
}
