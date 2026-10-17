// SPDX-License-Identifier: MIT
/*
 * Ozzy USB Audio Driver - device settings as ALSA controls on the engine
 *
 * One enumerated control per setting the device has, named as common/ozzy_settings.h names
 * it (ozzyctl finds them by name), plus "S/PDIF Clock Validity" with the clock source.
 * A route applies at once (the engine routes from the next packet); a device setting
 * restarts the engine, whose handshake sends it.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include <sound/control.h>

#include "ozzy.h"

static int setting_info(struct snd_kcontrol *kctl, struct snd_ctl_elem_info *uinfo)
{
	struct ozzy_chip *chip = snd_kcontrol_chip(kctl);
	const struct ozzy_setting_desc *desc = &ozzy_setting_descs[kctl->private_value];

	return snd_ctl_enum_info(uinfo, 1, ozzy_engine_setting_values(chip->engine, kctl->private_value), desc->value_names);
}

static int setting_get(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol)
{
	struct ozzy_chip *chip = snd_kcontrol_chip(kctl);

	ucontrol->value.enumerated.item[0] = chip->engine->settings.value[kctl->private_value];
	return 0;
}

/* 1 if the value changed, 0 if not, negative errno if the restart failed (the value stays) */
static int setting_put(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol)
{
	struct ozzy_chip *chip = snd_kcontrol_chip(kctl);
	enum ozzy_setting_change change;
	int ret = 0;

	mutex_lock(&chip->lock);
	change = ozzy_engine_change_setting(chip->engine, kctl->private_value, ucontrol->value.enumerated.item[0]);
	if (change == OZZY_SETTING_RESTART && chip->running)
		ret = ozzy_chip_restart(chip);
	mutex_unlock(&chip->lock);
	if (change == OZZY_SETTING_INVALID)
		return -EINVAL;
	if (change == OZZY_SETTING_SAME)
		return 0;

	if (chip->clock_lock_kctl)
		snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE, &chip->clock_lock_kctl->id);
	return ret < 0 ? ret : 1;
}

static int clock_lock_get(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol)
{
	struct ozzy_chip *chip = snd_kcontrol_chip(kctl);

	ucontrol->value.integer.value[0] = chip->shm->audio.settings.clock_locked;
	return 0;
}

static const struct snd_kcontrol_new clock_lock_ctl = {
	.iface  = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name   = "S/PDIF Clock Validity",
	.access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
	.info   = snd_ctl_boolean_mono_info,
	.get    = clock_lock_get,
};

int ozzy_controls_init(struct ozzy_chip *chip)
{
	struct snd_kcontrol_new tmpl = {
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.info  = setting_info,
		.get   = setting_get,
		.put   = setting_put,
	};
	struct snd_kcontrol *kctl;
	unsigned int id;
	int ret;

	for (id = 0; id < OZZY_NUM_SETTINGS; id++) {
		if (!ozzy_engine_has_setting(chip->engine, id))
			continue;
		tmpl.name = ozzy_setting_descs[id].name;
		tmpl.private_value = id;
		ret = snd_ctl_add(chip->card, snd_ctl_new1(&tmpl, chip));
		if (ret < 0)
			return ret;
	}
	if (ozzy_engine_has_setting(chip->engine, OZZY_SETTING_CLOCK_SOURCE)) {
		kctl = snd_ctl_new1(&clock_lock_ctl, chip);
		ret = snd_ctl_add(chip->card, kctl);
		if (ret < 0)
			return ret;
		chip->clock_lock_kctl = kctl;
	}
	return 0;
}
