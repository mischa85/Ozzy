/* SPDX-License-Identifier: MIT */
/*
 * Ozzy control: list Ozzy devices, show or change their settings (common/ozzy_settings.h) and
 * hear when they change, for the front ends: ozzy-cli (cli/) and the ozzy app (gui/).
 *
 * The core (ozzy_control_core.c: names, lookups) is the same on every platform; a backend per
 * platform finds the devices, reads and writes their settings and tells when they change:
 *   - macOS: backend_macos.c, OzzyHAL's OZZY_PROPERTY_SETTING through CoreAudio
 *   - Linux: backend_linux.c, the driver's ALSA controls through alsa-lib
 *   - Windows: backend_windows.c, the ACX driver's property set (windows/ozzy_ksproperty.h)
 *     through IOCTL_KS_PROPERTY
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_CONTROL_H
#define OZZY_CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../../common/ozzy_settings.h"
#include "../../engine/shared/ozzy_info.h"

#define OZZY_CONTROL_MAX_DEVICES 16

/*
 * struct ozzy_control_host - What the OS's audio stack has of the device, beside the driver's
 * own record; 0 / empty where the platform doesn't say. Arrays by direction: [0] out, [1] in.
 * @driver:        which driver the backend talks to ("snd-usb-ozzy", ...)
 * @running:       a client streams now (CoreAudio: IsRunningSomewhere; ALSA: a stream open)
 * @buffer_frames: CoreAudio's IO buffer, or the ALSA period, in frames
 * @ring_frames:   the ALSA buffer, in frames
 * @latency, @safety_offset: what the driver reports to CoreAudio, in frames
 * @timestamp_period: CoreAudio's zero timestamp period, in frames
 * @format:        the stream format as the OS names it ("S24_3LE")
 */
struct ozzy_control_host {
	char driver[32];
	bool running;
	uint32_t buffer_frames;
	uint32_t ring_frames;
	uint32_t latency[2];
	uint32_t safety_offset[2];
	uint32_t timestamp_period;
	char format[64];
};

/*
 * struct ozzy_control_device - One device as the backend found it.
 * @name:          product name
 * @id:            the backend's stable id (macOS: the CoreAudio device UID, Linux: the card id,
 *                 Windows: the device instance ID)
 * @sample_rate:   the rate it runs at (Hz), 0 if unknown (Linux: while no stream is open)
 * @in_channels, @out_channels: channel counts (route settings depend on them)
 * @available:     bit (1 << enum ozzy_setting) per setting the device has
 * @settings:      their values
 * @has_clock:     @clock is valid (the device has the clock source; lock_rate 0: unknown)
 * @has_info:      @info is the driver's record (a driver from before it had one: false)
 * @host:          what the OS's audio stack says
 * @handle:        backend-private
 */
struct ozzy_control_device {
	char name[128];
	char id[160];
	uint32_t sample_rate;
	unsigned int in_channels;
	unsigned int out_channels;
	uint32_t available;
	struct ozzy_settings settings;
	bool has_clock;
	struct ozzy_clock_status clock;
	bool has_info;
	struct ozzy_info info;
	struct ozzy_control_host host;
	uintptr_t handle;
};

/*
 * ozzy_control_backend_list - Fill @devs with up to @max devices. Returns how many, or -1 if the
 * backend can't look (it prints why).
 */
int ozzy_control_backend_list(struct ozzy_control_device *devs, int max);

/*
 * ozzy_control_backend_set - Set setting @id of @dev to @value. Returns 0, or -1 (it prints why).
 * A device setting may take effect a moment later; the core reads back to wait for it.
 */
int ozzy_control_backend_set(const struct ozzy_control_device *dev, unsigned int id, unsigned int value);

/*
 * ozzy_control_backend_watch - From now on, call @changed(@ctx) after a device arrives or leaves, or a
 * device's settings, rate or clock lock change; list again to see what. Notified by the OS and
 * the driver, never polled: macOS CoreAudio property listeners, Linux ALSA control events and
 * inotify on /dev/snd, Windows the driver's KS event and CM notifications. @changed runs on a
 * backend thread, one call at a time, and may list. Call once per process. Returns 0, or -1
 * (it prints why).
 */
int ozzy_control_backend_watch(void (*changed)(void *ctx), void *ctx);

/* ozzy_control_core.c: what every front end needs on top of the backend */

/* @name in lower case with runs of other characters as one dash ("S/PDIF" -> "s-pdif") */
void ozzy_control_slug(const char *name, char *out, size_t size);

/* How many values setting @id of @dev has: 0 if it doesn't have it (or channel counts unknown) */
unsigned int ozzy_control_values(const struct ozzy_control_device *dev, unsigned int id);
/* Whether @dev can take value @value of setting @id: transfer sizes are whole packets of its
 * endpoints (ozzy_pcm_step, from the driver's record); every value when that isn't known */
bool ozzy_control_value_fits(const struct ozzy_control_device *dev, unsigned int id, unsigned int value);

/* The device @arg picks (a number from the list or part of the name, NULL: the first), or NULL */
const struct ozzy_control_device *ozzy_control_pick(const struct ozzy_control_device *devs, int n, const char *arg);

/* The setting @arg names on @dev (its name, slug or number), or -1 */
int ozzy_control_find_setting(const struct ozzy_control_device *dev, const char *arg);

/* The value @arg names of setting @id on @dev (its name, slug or number), or -1 */
int ozzy_control_find_value(const struct ozzy_control_device *dev, unsigned int id, const char *arg);

/* The device with backend id @id in a fresh list, into @out; false if gone */
bool ozzy_control_reread(const char *id, struct ozzy_control_device *out);

/* Whether @a and @b show the same (everything but the backend's handle) */
bool ozzy_control_same(const struct ozzy_control_device *a, const struct ozzy_control_device *b);

/* @raw (@size bytes, from the driver) into @out if it is a struct ozzy_info this build reads */
bool ozzy_control_take_info(struct ozzy_info *out, const void *raw, size_t size);

/*
 * What ozzy-cli and the ozzy app show of a device beside its settings: facts, each a label and
 * its value as text, in groups, in the order to show them.
 */
enum ozzy_control_group {
	OZZY_CONTROL_GROUP_DEVICE,
	OZZY_CONTROL_GROUP_AUDIO,
	OZZY_CONTROL_GROUP_STREAM,
	OZZY_CONTROL_GROUP_ENDPOINTS,
	OZZY_CONTROL_GROUP_HOST,
	OZZY_CONTROL_GROUPS
};

extern const char *const ozzy_control_group_names[OZZY_CONTROL_GROUPS];

struct ozzy_control_fact {
	enum ozzy_control_group group;
	char label[32];
	char value[112];
};

#define OZZY_CONTROL_MAX_FACTS 48

/* @dev's facts into @out (up to @max), grouped and in order. Returns how many. */
int ozzy_control_facts(const struct ozzy_control_device *dev, struct ozzy_control_fact *out, int max);

#endif /* OZZY_CONTROL_H */
