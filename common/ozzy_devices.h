/* SPDX-License-Identifier: MIT */
/*
 * Ozzy Device List: the table built from ozzy_devices.def
 *
 * ozzy_devices.def has one OZZY_DEVICE(...) row per device. Each consumer
 * defines OZZY_DEVICE to what it needs and includes the .def:
 *   - this header: the shared table (ozzy_device_lookup), used for channels,
 *     rates, names and the family on every platform
 *   - linux/ozzy_core.c: the USB ID table (rows of the families Linux drives)
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

/* Quirks (quirks field bits): none yet */

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
	uint32_t quirks;      /* OZZY_QUIRK_xxx bits */
};

static const struct ozzy_device ozzy_devices[] = {
#define OZZY_DEVICE(key_, vid_, pid_, name_, family_, in_, out_, rates_, status_, quirks_) \
	{ #key_, name_, vid_, pid_, OZZY_FAMILY_##family_, in_, out_, OZZY_STATUS_##status_, \
	  rates_, quirks_ },
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

#ifdef __cplusplus
}
#endif

#endif /* OZZY_DEVICES_H */
