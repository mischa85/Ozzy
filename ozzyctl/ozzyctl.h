/* SPDX-License-Identifier: MIT */
/*
 * ozzyctl: list Ozzy devices and show or change their settings (common/ozzy_settings.h)
 *
 * The core (ozzyctl.c: commands, names, output) is the same on every platform; a backend
 * per platform finds the devices and reads and writes their settings:
 *   - macOS: backend_macos.c, OzzyHAL's kOzzyPropertySetting through CoreAudio
 *   - Linux: backend_linux.c, the driver's ALSA controls through alsa-lib
 *   - Windows: later
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZYCTL_H
#define OZZYCTL_H

#include <stdbool.h>
#include <stdint.h>
#include "../common/ozzy_settings.h"

#define OZZYCTL_MAX_DEVICES 16

/*
 * struct ozzyctl_device - One device as the backend found it.
 * @name:          product name
 * @id:            the backend's stable id (macOS: the CoreAudio device UID, Linux: the card id)
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

#endif /* OZZYCTL_H */
