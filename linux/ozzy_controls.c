// SPDX-License-Identifier: MIT
/*
 * Ozzy USB Audio Driver - Device Settings Controls
 *
 * One enumerated ALSA control per device setting (common/ozzy_settings.h), named
 * like snd-usb-audio's where it has one ("Clock Source": its clock selectors are
 * "<name> Clock Source"), plus a read-only "S/PDIF Clock Validity" for devices with
 * the clock source (its clock validity controls are "<clock name> Validity").
 * A changed device setting stops running streams with an xrun and resets the device;
 * the post-reset handshake sends it (ozzy_pcm_reconfigure). A changed route only
 * updates chip->in_route / out_route, which the PCM paths apply from the next packet.
 * alsactl store/restore keeps the values across reboots.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include <linux/usb.h>
#include <sound/core.h>
#include <sound/control.h>

#include "ozzy.h"
#include "ozzy_log.h"
#include "ozzy_pcm.h"
#include "ozzy_controls.h"

static unsigned int ozzy_setting_count_for(const struct ozzy_chip *chip, unsigned int id)
{
	return ozzy_setting_count(id, chip->info->capture_channels, chip->info->playback_channels);
}

/*
 * ozzy_controls_update_routes - The routing from the route settings. The PCM paths read
 * it without a lock: a packet during a change may mix the old and new routing.
 */
static void ozzy_controls_update_routes(struct ozzy_chip *chip)
{
	ozzy_settings_route(&chip->in_route, &chip->settings, chip->settings_available, true,
			    chip->info->capture_channels);
	ozzy_settings_route(&chip->out_route, &chip->settings, chip->settings_available, false,
			    chip->info->playback_channels);
}

static int ozzy_setting_info(struct snd_kcontrol *kctl, struct snd_ctl_elem_info *uinfo)
{
	struct ozzy_chip *chip = snd_kcontrol_chip(kctl);
	const struct ozzy_setting_desc *desc = &ozzy_setting_descs[kctl->private_value];

	return snd_ctl_enum_info(uinfo, 1, ozzy_setting_count_for(chip, kctl->private_value),
				 desc->value_names);
}

static int ozzy_setting_get(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol)
{
	struct ozzy_chip *chip = snd_kcontrol_chip(kctl);

	ucontrol->value.enumerated.item[0] = chip->settings.value[kctl->private_value];
	return 0;
}

/*
 * ozzy_setting_put - Store the new value and apply it: a route at once, a device setting
 * by resetting the device. Returns 1 if the value changed, 0 if not, negative errno if
 * the reset failed (the value stays stored; the next reset sends it).
 */
static int ozzy_setting_put(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol)
{
	struct ozzy_chip *chip = snd_kcontrol_chip(kctl);
	unsigned int id = kctl->private_value;
	unsigned int value = ucontrol->value.enumerated.item[0];
	int ret;

	if (value >= ozzy_setting_count_for(chip, id))
		return -EINVAL;

	mutex_lock(&chip->settings_mutex);
	if (chip->settings.value[id] == value) {
		mutex_unlock(&chip->settings_mutex);
		return 0;
	}
	chip->settings.value[id] = value;
	ozzy_log(&chip->dev->dev, "%s: %s\n", ozzy_setting_descs[id].name,
		 ozzy_setting_descs[id].value_names[value]);
	if (ozzy_setting_descs[id].kind != OZZY_SETTING_DEVICE) {
		ozzy_controls_update_routes(chip);
		mutex_unlock(&chip->settings_mutex);
		return 1;
	}
	ret = ozzy_pcm_reconfigure(chip);
	mutex_unlock(&chip->settings_mutex);

	if (chip->clock_lock_kctl)
		snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE, &chip->clock_lock_kctl->id);
	return ret < 0 ? ret : 1;
}

static int ozzy_clock_lock_get(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol)
{
	struct ozzy_chip *chip = snd_kcontrol_chip(kctl);

	ucontrol->value.integer.value[0] = chip->clock.locked;
	return 0;
}

static const struct snd_kcontrol_new ozzy_clock_lock_ctl = {
	.iface  = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name   = "S/PDIF Clock Validity",
	.access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
	.info   = snd_ctl_boolean_mono_info,
	.get    = ozzy_clock_lock_get,
};

int ozzy_controls_init(struct ozzy_chip *chip)
{
	struct snd_kcontrol_new tmpl = {
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.info  = ozzy_setting_info,
		.get   = ozzy_setting_get,
		.put   = ozzy_setting_put,
	};
	struct snd_kcontrol *kctl;
	unsigned int id;
	int ret;

	for (id = 0; id < OZZY_NUM_SETTINGS; id++)
		if (ozzy_setting_values(chip->quirks, chip->info->capture_channels,
					chip->info->playback_channels, id))
			chip->settings_available |= 1u << id;
	ozzy_controls_update_routes(chip);

	for (id = 0; id < OZZY_NUM_SETTINGS; id++) {
		if (!(chip->settings_available & (1u << id)))
			continue;
		tmpl.name = ozzy_setting_descs[id].name;
		tmpl.private_value = id;
		ret = snd_ctl_add(chip->card, snd_ctl_new1(&tmpl, chip));
		if (ret < 0)
			return ret;
	}

	if (chip->settings_available & (1u << OZZY_SETTING_CLOCK_SOURCE)) {
		kctl = snd_ctl_new1(&ozzy_clock_lock_ctl, chip);
		ret = snd_ctl_add(chip->card, kctl);
		if (ret < 0)
			return ret;
		chip->clock_lock_kctl = kctl;
	}
	return 0;
}
