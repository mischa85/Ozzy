/* SPDX-License-Identifier: MIT */
/*
 * Ozzy Device Settings: the settings model shared by every platform
 *
 * The user-facing settings a device can have (clock source first; S/PDIF mode, digital
 * out and routing later), their values and names. Which device has which setting
 * follows its quirk bits (ozzy_quirks.h). The control requests each value sends live
 * in the family's protocol code (ploytec_handshake.c); platforms only map the settings
 * onto their own tooling:
 *   - Linux: one ALSA kcontrol per setting, named after desc->name
 *   - macOS: CoreAudio properties in OzzyHAL (clock source: kAudioDevicePropertyClockSource),
 *     sent to the kext with kOzzyMethodApplySetting
 *
 * Pure C with no platform dependencies -- usable in both kernel space
 * and userspace on any platform.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_SETTINGS_H
#define OZZY_SETTINGS_H

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

enum ozzy_setting {
	OZZY_SETTING_CLOCK_SOURCE,
	OZZY_NUM_SETTINGS
};

/* OZZY_SETTING_CLOCK_SOURCE values */
enum ozzy_clock_source {
	OZZY_CLOCK_INTERNAL = 0,
	OZZY_CLOCK_SPDIF    = 1,
	OZZY_NUM_CLOCK_SOURCES
};

/*
 * struct ozzy_setting_desc - One setting: its name, values and which devices have it.
 * @name:        the setting's name, also the ALSA control name ("Clock Source", as
 *               snd-usb-audio names its clock selector)
 * @value_names: @num_values names, indexed by value
 * @def:         the value a device starts with (what the handshake sent before
 *               settings existed)
 * @quirk:       the device has the setting if its quirks have this bit
 */
struct ozzy_setting_desc {
	const char *name;
	const char *const *value_names;
	uint8_t num_values;
	uint8_t def;
	uint32_t quirk;
};

static const char *const ozzy_clock_source_names[OZZY_NUM_CLOCK_SOURCES] = { "Internal", "S/PDIF" };

static const struct ozzy_setting_desc ozzy_setting_descs[OZZY_NUM_SETTINGS] = {
	/* Positional: the macOS kext compiles this as C++11 */
	{ "Clock Source", ozzy_clock_source_names, OZZY_NUM_CLOCK_SOURCES, OZZY_CLOCK_INTERNAL,
	  OZZY_QUIRK_DIGITAL_IN },
};

/*
 * struct ozzy_settings - What the user chose, one value per enum ozzy_setting.
 * Platforms keep it across device resets and engine restarts.
 */
struct ozzy_settings {
	uint8_t value[OZZY_NUM_SETTINGS];
};

/*
 * struct ozzy_clock_status - What the device reported about its digital input clock,
 * at the last handshake (the official driver only reads it there too).
 * @locked:    S/PDIF is the clock source and the device is locked to it
 * @lock_rate: the rate it locked to (Hz), 0 when not locked
 */
struct ozzy_clock_status {
	bool locked;
	uint32_t lock_rate;
};

static inline void ozzy_settings_defaults(struct ozzy_settings *s)
{
	unsigned int i;

	for (i = 0; i < OZZY_NUM_SETTINGS; i++)
		s->value[i] = ozzy_setting_descs[i].def;
}

/*
 * ozzy_has_setting - Whether a device with @quirks (its ozzy_devices.def row) has setting @id.
 */
static inline bool ozzy_has_setting(uint32_t quirks, unsigned int id)
{
	return id < OZZY_NUM_SETTINGS && (quirks & ozzy_setting_descs[id].quirk);
}

/*
 * ozzy_setting_valid - Whether @value is one of setting @id's values.
 */
static inline bool ozzy_setting_valid(unsigned int id, unsigned int value)
{
	return id < OZZY_NUM_SETTINGS && value < ozzy_setting_descs[id].num_values;
}

#ifdef __cplusplus
}
#endif

#endif /* OZZY_SETTINGS_H */
