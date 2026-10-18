/* SPDX-License-Identifier: MIT */
/*
 * ozzyctl: list Ozzy devices and show or change their settings (common/ozzy_settings.h)
 *
 * The core (ozzyctl_core.c: names, lookups) is the same on every platform and shared by every
 * front end (ozzyctl.c, the console tool); a backend per platform finds the devices, reads and
 * writes their settings and tells when they change:
 *   - macOS: backend_macos.c, OzzyHAL's OZZY_PROPERTY_SETTING through CoreAudio
 *   - Linux: backend_linux.c, the driver's ALSA controls through alsa-lib
 *   - Windows: backend_windows.c, the ACX driver's property set (windows/ozzy_ksproperty.h)
 *     through IOCTL_KS_PROPERTY
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZYCTL_H
#define OZZYCTL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../common/ozzy_settings.h"

#define OZZYCTL_MAX_DEVICES 16

/*
 * struct ozzyctl_device - One device as the backend found it.
 * @name:          product name
 * @id:            the backend's stable id (macOS: the CoreAudio device UID, Linux: the card id,
 *                 Windows: the device instance ID)
 * @sample_rate:   the rate it runs at (Hz), 0 if unknown (Linux: while no stream is open)
 * @in_channels, @out_channels: channel counts (route settings depend on them)
 * @available:     bit (1 << enum ozzy_setting) per setting the device has
 * @settings:      their values
 * @has_clock:     @clock is valid (the device has the clock source; lock_rate 0: unknown)
 * @handle:        backend-private
 */
struct ozzyctl_device {
	char name[128];
	char id[160];
	uint32_t sample_rate;
	unsigned int in_channels;
	unsigned int out_channels;
	uint32_t available;
	struct ozzy_settings settings;
	bool has_clock;
	struct ozzy_clock_status clock;
	uintptr_t handle;
};

/*
 * ozzyctl_backend_list - Fill @devs with up to @max devices. Returns how many, or -1 if the
 * backend can't look (it prints why).
 */
int ozzyctl_backend_list(struct ozzyctl_device *devs, int max);

/*
 * ozzyctl_backend_set - Set setting @id of @dev to @value. Returns 0, or -1 (it prints why).
 * A device setting may take effect a moment later; the core reads back to wait for it.
 */
int ozzyctl_backend_set(const struct ozzyctl_device *dev, unsigned int id, unsigned int value);

/*
 * ozzyctl_backend_watch - From now on, call @changed(@ctx) after a device arrives or leaves, or a
 * device's settings, rate or clock lock change; list again to see what. Notified by the OS and
 * the driver, never polled: macOS CoreAudio property listeners, Linux ALSA control events and
 * inotify on /dev/snd, Windows the driver's KS event and CM notifications. @changed runs on a
 * backend thread, one call at a time, and may list. Call once per process. Returns 0, or -1
 * (it prints why).
 */
int ozzyctl_backend_watch(void (*changed)(void *ctx), void *ctx);

/* ozzyctl_core.c: what every front end needs on top of the backend */

/* @name in lower case with runs of other characters as one dash ("S/PDIF" -> "s-pdif") */
void ozzyctl_slug(const char *name, char *out, size_t size);

/* How many values setting @id of @dev has: 0 if it doesn't have it (or channel counts unknown) */
unsigned int ozzyctl_values(const struct ozzyctl_device *dev, unsigned int id);

/* The device @arg picks (a number from the list or part of the name, NULL: the first), or NULL */
const struct ozzyctl_device *ozzyctl_pick(const struct ozzyctl_device *devs, int n, const char *arg);

/* The setting @arg names on @dev (its name, slug or number), or -1 */
int ozzyctl_find_setting(const struct ozzyctl_device *dev, const char *arg);

/* The value @arg names of setting @id on @dev (its name, slug or number), or -1 */
int ozzyctl_find_value(const struct ozzyctl_device *dev, unsigned int id, const char *arg);

/* The device with backend id @id in a fresh list, into @out; false if gone */
bool ozzyctl_reread(const char *id, struct ozzyctl_device *out);

/* Whether @a and @b show the same (everything but the backend's handle) */
bool ozzyctl_same(const struct ozzyctl_device *a, const struct ozzyctl_device *b);

#endif /* OZZYCTL_H */
