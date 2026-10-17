/* SPDX-License-Identifier: MIT */
/*
 * Ozzy Device Settings: the settings model shared by every platform
 *
 * The user-facing settings a device can have, their values and names. Which device has
 * which setting follows its quirk bits (ozzy_quirks.h) and, for routing, its channel
 * counts. Two kinds:
 *   - device settings (clock source, S/PDIF out mode): the family's protocol code sends
 *     them in its handshake (ploytec_handshake.c), so a change restarts streaming
 *   - routes and output pairs: which stereo pair goes where, applied by the host's codecs
 *     (ozzy_route.h, the family's encoder), no request and no restart
 * Platforms only map the settings onto their own tooling:
 *   - Linux: one ALSA kcontrol per setting, named after desc->name
 *   - macOS: OzzyHAL; the clock source as kAudioDevicePropertyClockSource, every setting as
 *     the custom property kOzzyPropertySetting; device settings go to the kext with
 *     kOzzyMethodApplySetting
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
#include "ozzy_route.h"

#ifdef __cplusplus
extern "C" {
#endif

enum ozzy_setting {
	OZZY_SETTING_CLOCK_SOURCE,
	OZZY_SETTING_SPDIF_OUT_MODE,
	OZZY_SETTING_OUT_ROUTE_1,     /* source of device output pair 1 (outputs 1-2) */
	OZZY_SETTING_OUT_ROUTE_2,
	OZZY_SETTING_IN_ROUTE_1,      /* source of host capture pair 1 (capture 1-2) */
	OZZY_SETTING_IN_ROUTE_2,
	OZZY_SETTING_IN_ROUTE_3,
	OZZY_SETTING_SPDIF_MIRROR,    /* the playback pair the S/PDIF output mirrors (ESU1808) */
	OZZY_NUM_SETTINGS
};

enum ozzy_setting_kind {
	OZZY_SETTING_DEVICE,          /* sent to the device in the handshake */
	OZZY_SETTING_OUT_ROUTE,       /* value: host playback pair */
	OZZY_SETTING_IN_ROUTE,        /* value: device input pair */
	OZZY_SETTING_OUT_PAIR,        /* value: host playback pair, used by the family's encoder */
};

/*
 * OZZY_SETTING_CLOCK_SOURCE values, the official driver's ClockSource (AjExtData[1]). Tascam's
 * US-1641/1800/2000 Control Panel offers "Automatic" (index 0, the default: the digital input
 * when it has a valid signal, else internal) and "Internal" (index 1: always internal), and
 * passes the index to setClockSource.
 */
enum ozzy_clock_source {
	OZZY_CLOCK_AUTOMATIC = 0,
	OZZY_CLOCK_INTERNAL  = 1,
	OZZY_NUM_CLOCK_SOURCES
};

/*
 * OZZY_SETTING_SPDIF_OUT_MODE values: the S/PDIF output's channel status (the official
 * driver's DigitalOutMode 0/1). Mode 1 sets bit 0 of channel status byte 0, IEC 60958's
 * professional flag, so it is named Professional (from the standard, not the driver).
 */
enum ozzy_spdif_out_mode {
	OZZY_SPDIF_OUT_CONSUMER     = 0,
	OZZY_SPDIF_OUT_PROFESSIONAL = 1,
	OZZY_NUM_SPDIF_OUT_MODES
};

/*
 * struct ozzy_setting_desc - One setting: its name, values and which devices have it.
 * @name:        the setting's name, also the ALSA control name ("Clock Source", as
 *               snd-usb-audio names its clock selector)
 * @value_names: names indexed by value (routes, output pairs: OZZY_ROUTE_MAX_PAIRS of them)
 * @num_values:  device settings: how many values; others: see ozzy_setting_count
 * @def:         the value a device starts with (device settings: what the handshake sent
 *               before settings existed; routes: the pair itself)
 * @quirk:       the device has the setting if its quirks have this bit
 * @kind:        enum ozzy_setting_kind
 * @pair:        routes: the pair this setting routes (0 = channels 1-2)
 */
struct ozzy_setting_desc {
	const char *name;
	const char *const *value_names;
	uint8_t num_values;
	uint8_t def;
	uint32_t quirk;
	uint8_t kind;
	uint8_t pair;
};

static const char *const ozzy_clock_source_names[OZZY_NUM_CLOCK_SOURCES] = { "Automatic", "Internal" };
static const char *const ozzy_spdif_out_mode_names[OZZY_NUM_SPDIF_OUT_MODES] = { "Consumer", "Professional" };
static const char *const ozzy_playback_pair_names[OZZY_ROUTE_MAX_PAIRS] = {
	"Playback 1-2", "Playback 3-4", "Playback 5-6", "Playback 7-8",
	"Playback 9-10", "Playback 11-12", "Playback 13-14", "Playback 15-16" };
static const char *const ozzy_input_pair_names[OZZY_ROUTE_MAX_PAIRS] = {
	"Input 1-2", "Input 3-4", "Input 5-6", "Input 7-8",
	"Input 9-10", "Input 11-12", "Input 13-14", "Input 15-16" };

static const struct ozzy_setting_desc ozzy_setting_descs[OZZY_NUM_SETTINGS] = {
	/* Positional: the macOS kext compiles this as C++11 */
	{ "Clock Source", ozzy_clock_source_names, OZZY_NUM_CLOCK_SOURCES, OZZY_CLOCK_AUTOMATIC,
	  OZZY_QUIRK_DIGITAL_IN, OZZY_SETTING_DEVICE, 0 },
	/* ALSA names S/PDIF "IEC958" (Documentation/sound/designs/control-names.rst) */
	{ "IEC958 Playback Mode", ozzy_spdif_out_mode_names, OZZY_NUM_SPDIF_OUT_MODES,
	  OZZY_SPDIF_OUT_CONSUMER, OZZY_QUIRK_SPDIF_OUT, OZZY_SETTING_DEVICE, 0 },
	{ "Output 1-2 Source", ozzy_playback_pair_names, OZZY_ROUTE_MAX_PAIRS, 0,
	  OZZY_QUIRK_OUT_ROUTING, OZZY_SETTING_OUT_ROUTE, 0 },
	{ "Output 3-4 Source", ozzy_playback_pair_names, OZZY_ROUTE_MAX_PAIRS, 1,
	  OZZY_QUIRK_OUT_ROUTING, OZZY_SETTING_OUT_ROUTE, 1 },
	{ "Capture 1-2 Source", ozzy_input_pair_names, OZZY_ROUTE_MAX_PAIRS, 0,
	  OZZY_QUIRK_IN_ROUTING, OZZY_SETTING_IN_ROUTE, 0 },
	{ "Capture 3-4 Source", ozzy_input_pair_names, OZZY_ROUTE_MAX_PAIRS, 1,
	  OZZY_QUIRK_IN_ROUTING, OZZY_SETTING_IN_ROUTE, 1 },
	{ "Capture 5-6 Source", ozzy_input_pair_names, OZZY_ROUTE_MAX_PAIRS, 2,
	  OZZY_QUIRK_IN_ROUTING, OZZY_SETTING_IN_ROUTE, 2 },
	{ "IEC958 Playback Source", ozzy_playback_pair_names, OZZY_ROUTE_MAX_PAIRS, 0,
	  OZZY_QUIRK_SPDIF_MIRROR, OZZY_SETTING_OUT_PAIR, 0 },
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
 * @locked:    clock source Automatic and the device is locked to its S/PDIF input
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
 * ozzy_has_setting - Whether a device with @quirks (its ozzy_devices.def row) has setting @id,
 * channel counts aside. Enough for device settings (the handshake uses it); routes also
 * depend on the channels: ozzy_setting_values.
 */
static inline bool ozzy_has_setting(uint32_t quirks, unsigned int id)
{
	return id < OZZY_NUM_SETTINGS && (quirks & ozzy_setting_descs[id].quirk);
}

/*
 * ozzy_setting_count - How many values setting @id has on a device with @in_ch / @out_ch
 * channels that has the setting: a route's values are the pairs on its source side, and a
 * route exists only for a pair the device has (0 then).
 */
static inline unsigned int ozzy_setting_count(unsigned int id, unsigned int in_ch, unsigned int out_ch)
{
	const struct ozzy_setting_desc *d;
	unsigned int pairs;

	if (id >= OZZY_NUM_SETTINGS)
		return 0;
	d = &ozzy_setting_descs[id];
	if (d->kind == OZZY_SETTING_DEVICE)
		return d->num_values;
	pairs = (d->kind == OZZY_SETTING_IN_ROUTE ? in_ch : out_ch) / 2;
	if (pairs > OZZY_ROUTE_MAX_PAIRS)
		pairs = OZZY_ROUTE_MAX_PAIRS;
	return d->pair < pairs ? pairs : 0;   /* OUT_PAIR: pair 0, any playback pair */
}

/*
 * ozzy_setting_values - How many values setting @id has on a device with @quirks and
 * @in_ch / @out_ch channels; 0 if it doesn't have it.
 */
static inline unsigned int ozzy_setting_values(uint32_t quirks, unsigned int in_ch,
					       unsigned int out_ch, unsigned int id)
{
	return ozzy_has_setting(quirks, id) ? ozzy_setting_count(id, in_ch, out_ch) : 0;
}

/*
 * ozzy_settings_route - One direction's routing (@input: capture, else playback) from the
 * route settings in @s, for a device with @channels channels in that direction. Pairs
 * without a route setting, or a device without @available, stay where they are.
 * @available: bit (1 << id) per setting the device has.
 */
static inline void ozzy_settings_route(struct ozzy_route *r, const struct ozzy_settings *s,
				       uint32_t available, bool input, unsigned int channels)
{
	uint8_t src[OZZY_ROUTE_MAX_PAIRS];
	unsigned int id, k;

	for (k = 0; k < OZZY_ROUTE_MAX_PAIRS; k++)
		src[k] = (uint8_t)k;
	for (id = 0; id < OZZY_NUM_SETTINGS; id++) {
		const struct ozzy_setting_desc *d = &ozzy_setting_descs[id];

		if ((available & (1u << id)) &&
		    d->kind == (input ? OZZY_SETTING_IN_ROUTE : OZZY_SETTING_OUT_ROUTE))
			src[d->pair] = s->value[id];
	}
	ozzy_route_set(r, channels, src);
}

#ifdef __cplusplus
}
#endif

#endif /* OZZY_SETTINGS_H */
