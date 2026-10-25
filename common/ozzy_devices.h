/* SPDX-License-Identifier: MIT */
/*
 * Ozzy Device List: the table built from ozzy_devices.def
 *
 * ozzy_devices.def has one OZZY_DEVICE(...) row per device. Each consumer
 * defines OZZY_DEVICE to what it needs and includes the .def:
 *   - this header: the shared table (ozzy_device_lookup), used for channels,
 *     rates, names and the family on every platform
 *   - linux/core.c: the USB ID table (rows of the families Linux drives)
 *   - macos/Backends/OzzyKext/GenPersonalities.c: the kext's matching
 *     personalities, generated into Info.plist
 * A row left out with -DOZZY_DISABLE_<key> is left out of all of them. Each
 * platform drives the families it has code for, so a new platform (Windows:
 * an INF generated like the kext's Info.plist) needs no change to the rows.
 *
 * Pure C with no platform dependencies -- usable in both kernel space
 * and userspace on any platform.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_DEVICES_H
#define OZZY_DEVICES_H

#if defined(__KERNEL__) && defined(__linux__)
#include <linux/types.h>
#else
#include <stdint.h>
#include <stdbool.h>
#endif

#include "ozzy_quirks.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Device families: which protocol code drives the device */
enum ozzy_family {
	OZZY_FAMILY_PLOYTEC_XONE,   /* Ploytec junction protocol, bulk/interrupt PCM out */
	OZZY_FAMILY_PLOYTEC_ISOC,   /* Ploytec bulk in / isochronous out */
	OZZY_FAMILY_USB_MIDI,       /* USB MIDI class */
};

enum ozzy_status {
	OZZY_STATUS_SUPPORTED,
	OZZY_STATUS_EXPERIMENTAL,
};

/* Sample rates (rates field bits), in ascending order */
#define OZZY_RATE_44K1   (1u << 0)
#define OZZY_RATE_48K    (1u << 1)
#define OZZY_RATE_88K2   (1u << 2)
#define OZZY_RATE_96K    (1u << 3)
#define OZZY_RATE_176K4  (1u << 4)
#define OZZY_RATE_192K   (1u << 5)
#define OZZY_MAX_RATES   6

/*
 * struct ozzy_device - One row of ozzy_devices.def.
 */
struct ozzy_device {
	const char *key;      /* identifier, e.g. "XoneDB4" (kext personality name) */
	const char *name;     /* product name, e.g. "Xone:DB4" */
	uint16_t vid;
	uint16_t pid;
	uint8_t family;       /* enum ozzy_family */
	uint8_t in_channels;
	uint8_t out_channels;
	uint8_t status;       /* enum ozzy_status */
	uint32_t rates;       /* OZZY_RATE_xxx bits */
	uint32_t quirks;      /* enum ozzy_quirk bits (ozzy_quirks.h) */
	uint16_t latency[2 * OZZY_MAX_RATES];  /* frames out, in, out, in, ... per rate of rates,
	                                         * ascending; 0: unknown */
};

#define OZZY_LATENCY(...)  { __VA_ARGS__ }
#define OZZY_NO_LATENCY    { 0 }

static const struct ozzy_device ozzy_devices[] = {
#define OZZY_DEVICE(key_, vid_, pid_, name_, family_, in_, out_, rates_, status_, quirks_, latency_) \
	{ #key_, name_, vid_, pid_, OZZY_FAMILY_##family_, in_, out_, OZZY_STATUS_##status_, \
	  rates_, quirks_, latency_ },
#include "ozzy_devices.def"
#undef OZZY_DEVICE
};

#define OZZY_NUM_DEVICES (sizeof(ozzy_devices) / sizeof(ozzy_devices[0]))

/*
 * ozzy_device_lookup - The row for a USB vendor/product ID, or NULL.
 */
static inline const struct ozzy_device *ozzy_device_lookup(uint16_t vid, uint16_t pid)
{
	unsigned int i;

	for (i = 0; i < OZZY_NUM_DEVICES; i++)
		if (ozzy_devices[i].vid == vid && ozzy_devices[i].pid == pid)
			return &ozzy_devices[i];
	return 0;
}

/*
 * ozzy_device_rates - A row's sample rates in Hz, ascending, into @out
 * (OZZY_MAX_RATES entries). Returns how many.
 */
static inline unsigned int ozzy_device_rates(const struct ozzy_device *dev, uint32_t *out)
{
	static const uint32_t hz[OZZY_MAX_RATES] = { 44100, 48000, 88200, 96000, 176400, 192000 };
	unsigned int i, n = 0;

	for (i = 0; i < OZZY_MAX_RATES; i++)
		if (dev->rates & (1u << i))
			out[n++] = hz[i];
	return n;
}

/*
 * ozzy_device_has_rate - Whether @dev lists @rate Hz (e.g. a rate it locked to over S/PDIF).
 */
static inline bool ozzy_device_has_rate(const struct ozzy_device *dev, uint32_t rate)
{
	uint32_t hz[OZZY_MAX_RATES];
	unsigned int i, n = ozzy_device_rates(dev, hz);

	for (i = 0; i < n; i++)
		if (hz[i] == rate)
			return true;
	return false;
}

/*
 * ozzy_device_latency - Frames @dev itself adds at @rate Hz on its output (@input false) or
 * input, beyond the transfers (the latency column of ozzy_devices.def); 0 where unknown.
 */
static inline uint32_t ozzy_device_latency(const struct ozzy_device *dev, uint32_t rate, bool input)
{
	uint32_t hz[OZZY_MAX_RATES];
	unsigned int i, n = dev ? ozzy_device_rates(dev, hz) : 0;

	for (i = 0; i < n; i++)
		if (hz[i] == rate)
			return dev->latency[2 * i + (input ? 1 : 0)];
	return 0;
}

/* Upper case of an ASCII letter (UTF-16 or char), anything else as it is */
static inline uint16_t ozzy_ascii_upper(uint16_t c)
{
	return c >= 'a' && c <= 'z' ? (uint16_t)(c - 'a' + 'A') : c;
}

/* Four hex digits at @s, or -1 */
static inline int ozzy_hex4_u16(const uint16_t *s)
{
	int v = 0, i;

	for (i = 0; i < 4; i++) {
		uint16_t c = ozzy_ascii_upper(s[i]);
		int d = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;

		if (d < 0)
			return -1;
		v = v * 16 + d;
	}
	return v;
}

/*
 * ozzy_device_lookup_hwid - The row for a Windows hardware ID, "USB\VID_xxxx&PID_yyyy..."
 * (UTF-16, any case), or NULL. NULL for an interface's ID (with "&MI_"): Ozzy drives whole
 * devices.
 */
static inline const struct ozzy_device *ozzy_device_lookup_hwid(const uint16_t *id)
{
	static const char vid_prefix[] = "USB\\VID_", pid_prefix[] = "&PID_", mi[] = "&MI_";
	unsigned int i, k;
	int vid, pid;

	for (i = 0; vid_prefix[i]; i++)
		if (ozzy_ascii_upper(id[i]) != (uint16_t)vid_prefix[i])
			return NULL;
	vid = ozzy_hex4_u16(id + 8);
	for (i = 0; vid >= 0 && pid_prefix[i]; i++)
		if (ozzy_ascii_upper(id[12 + i]) != (uint16_t)pid_prefix[i])
			return NULL;
	pid = vid >= 0 ? ozzy_hex4_u16(id + 17) : -1;
	if (vid < 0 || pid < 0)
		return NULL;
	for (i = 21; id[i]; i++) {
		for (k = 0; mi[k] && ozzy_ascii_upper(id[i + k]) == (uint16_t)mi[k]; k++)
			;
		if (!mi[k])
			return NULL;
	}
	return ozzy_device_lookup((uint16_t)vid, (uint16_t)pid);
}

/*
 * ozzy_device_host_rates - The rates a host may pick now, ascending, into @out (OZZY_MAX_RATES
 * entries): while the device is locked to S/PDIF at @lock_rate Hz only that one (the official
 * driver offers CoreAudio only the current rate then), else every rate @dev lists. @lock_rate
 * 0: not locked. Returns how many.
 */
static inline unsigned int ozzy_device_host_rates(const struct ozzy_device *dev, uint32_t lock_rate, uint32_t *out)
{
	if (lock_rate) {
		out[0] = lock_rate;
		return 1;
	}
	return ozzy_device_rates(dev, out);
}

#ifdef __cplusplus
}
#endif

#endif /* OZZY_DEVICES_H */
