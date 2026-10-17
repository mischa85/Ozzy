// SPDX-License-Identifier: MIT
/*
 * ozzyctl backend for Windows: the Ozzy ACX driver's property set (windows/ozzy_ksproperty.h)
 * through IOCTL_KS_PROPERTY. Each device has a KSCATEGORY_AUDIO interface per endpoint; the
 * first one that answers the property stands for the device (the driver serves it on both).
 * Devices with another driver (the official one) don't answer, and aren't listed.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include <windows.h>
#include <setupapi.h>
#include <ks.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ozzyctl.h"
#include "../common/ozzy_devices.h"
#include "../windows/ozzy_ksproperty.h"

/* KSCATEGORY_AUDIO, here so no GUID library is needed */
static const GUID ksaudio = { 0x6994ad04, 0x93ef, 0x11d0, { 0xa3, 0xcc, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96 } };
static const GUID ozzy_propsetid_settings = OZZY_PROPSETID_SETTINGS_INIT;

/* Interface paths of the listed devices, by ozzyctl_device.handle */
static wchar_t paths[OZZYCTL_MAX_DEVICES][512];

/* One property call on the filter at @path: get into @value, or set from it */
static bool ks_property(const wchar_t *path, ULONG id, ULONG flags, void *value, DWORD size, DWORD *returned)
{
	KSPROPERTY prop;
	HANDLE h;
	BOOL ok;

	h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
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

int ozzyctl_backend_list(struct ozzyctl_device *devs, int max)
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
	for (i = 0; n < max && n < OZZYCTL_MAX_DEVICES && SetupDiEnumDeviceInterfaces(set, NULL, &ksaudio, i, &ifd); i++) {
		SP_DEVINFO_DATA dev = { sizeof(dev) };
		SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail;
		struct ozzyctl_device *d = &devs[n];
		struct ozzy_ksproperty_status st;
		const struct ozzy_device *model;
		wchar_t ids[1024] = { 0 }, instance[200], friendly[128] = { 0 };
		DWORD needed = 0, returned = 0;
		unsigned int id;
		bool dup = false;
		int k;

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
		wcsncpy_s(paths[n], 512, detail->DevicePath, _TRUNCATE);
		free(detail);

		if (SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_FRIENDLYNAME, NULL, (BYTE *)friendly,
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
		d->clock.lock_rate = st.clock_locked ? st.lock_rate : 0;
		n++;
	}
	SetupDiDestroyDeviceInfoList(set);
	return n;
}

int ozzyctl_backend_set(const struct ozzyctl_device *dev, unsigned int id, unsigned int value)
{
	struct ozzy_ksproperty_setting s = { id, value };
	DWORD returned = 0;

	/* A device setting restarts the device inside this call (streams glitch) */
	if (dev->handle >= OZZYCTL_MAX_DEVICES ||
	    !ks_property(paths[dev->handle], OZZY_KSPROPERTY_SETTING, KSPROPERTY_TYPE_SET, &s, sizeof(s), &returned)) {
		fprintf(stderr, "%s: setting %s failed (error %lu)\n", dev->name, ozzy_setting_descs[id].name,
			GetLastError());
		return -1;
	}
	return 0;
}
