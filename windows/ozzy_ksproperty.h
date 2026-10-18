/* SPDX-License-Identifier: MIT */
/*
 * The Windows driver's settings, for ozzy-cli and the ozzy app (apps/control/backend_windows.c): a KS
 * property set on each circuit's filter (the endpoints' KSCATEGORY_AUDIO device interfaces), read
 * and written with IOCTL_KS_PROPERTY. Windows has no place of its own for a clock source, an S/PDIF mode or
 * routing, so this is how they are reached. The same header builds in the driver and in user
 * mode.
 *
 * A client that keeps a filter open learns of changes from a KS event on it
 * (IOCTL_KS_ENABLE_EVENT with OZZY_EVENTSETID_SETTINGS, OZZY_KSEVENT_CHANGED).
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_KSPROPERTY_H
#define OZZY_KSPROPERTY_H

#include "../common/ozzy_settings.h"
#include "../engine/shared/ozzy_info.h"

/* The property set: {5499d49d-b94c-4c8b-b547-08b3a0e494aa}, as a GUID initializer */
#define OZZY_PROPSETID_SETTINGS_INIT \
	{ 0x5499d49d, 0xb94c, 0x4c8b, { 0xb5, 0x47, 0x08, 0xb3, 0xa0, 0xe4, 0x94, 0xaa } }

enum ozzy_ksproperty_id {
	OZZY_KSPROPERTY_STATUS = 0,    /* get: struct ozzy_ksproperty_status */
	OZZY_KSPROPERTY_SETTING = 1,   /* set: struct ozzy_ksproperty_setting */
	OZZY_KSPROPERTY_INFO = 2,      /* get: struct ozzy_info (engine/shared/ozzy_info.h) */
};

/* The event set: {1fd13819-4ef9-40f0-bdf3-695f6589d659}, as a GUID initializer */
#define OZZY_EVENTSETID_SETTINGS_INIT \
	{ 0x1fd13819, 0x4ef9, 0x40f0, { 0xbd, 0xf3, 0x69, 0x5f, 0x65, 0x89, 0xd6, 0x59 } }

enum ozzy_ksevent_id {
	OZZY_KSEVENT_CHANGED = 0,      /* a setting, the rate or the clock lock changed: get the status */
};

/* Bump when struct ozzy_ksproperty_status changes */
#define OZZY_KSPROPERTY_VERSION 2   /* 2: the PCM transfer size settings (value[] grew) */

/*
 * struct ozzy_ksproperty_status - The device as the engine has it.
 * @available:    bit (1 << enum ozzy_setting) per setting the device has
 * @value:        each setting's value (what the next handshake sends, for device settings)
 * @clock_locked: locked to S/PDIF at the last handshake, at @lock_rate Hz
 */
struct ozzy_ksproperty_status {
	uint32_t version;
	uint32_t sample_rate;
	uint32_t in_channels;
	uint32_t out_channels;
	uint32_t available;
	uint8_t value[OZZY_NUM_SETTINGS];
	uint8_t clock_locked;
	uint32_t lock_rate;
};

/*
 * struct ozzy_ksproperty_setting - Setting @id to @value. A route applies at once; a device setting
 * restarts the engine (streams glitch) and is remembered in the device's registry key.
 */
struct ozzy_ksproperty_setting {
	uint32_t id;
	uint32_t value;
};

#endif /* OZZY_KSPROPERTY_H */
