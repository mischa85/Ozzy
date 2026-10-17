// SPDX-License-Identifier: MIT
/*
 * Ozzy USB Audio Driver - device settings as ALSA controls on the engine (make OZZY_ENGINE=1)
 *
 * One enumerated control per setting the device has (common/ozzy_settings.h), named as in
 * the Linux driver (ozzy_controls.c), plus "S/PDIF Clock Validity" with the clock source.
 * A route applies at once (the engine routes from the next packet); a device setting
 * restarts the engine, whose handshake sends it.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include <sound/control.h>

#include "engine_chip.h"

static unsigned int esetting_count(const struct ozzy_echip *chip, unsigned int id)
{
	return ozzy_setting_values(ozzy_engine_setting_quirks(chip->engine), chip->model->in_channels,
				   chip->model->out_channels, id);
}

static int esetting_info(struct snd_kcontrol *kctl, struct snd_ctl_elem_info *uinfo)
{
	struct ozzy_echip *chip = snd_kcontrol_chip(kctl);
	const struct ozzy_setting_desc *desc = &ozzy_setting_descs[kctl->private_value];

	return snd_ctl_enum_info(uinfo, 1, esetting_count(chip, kctl->private_value), desc->value_names);
}

static int esetting_get(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol)
{
	struct ozzy_echip *chip = snd_kcontrol_chip(kctl);

	ucontrol->value.enumerated.item[0] = chip->engine->settings.value[kctl->private_value];
	return 0;
}

/* 1 if the value changed, 0 if not, negative errno if the restart failed (the value stays) */
static int esetting_put(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol)
{
	struct ozzy_echip *chip = snd_kcontrol_chip(kctl);
	unsigned int id = kctl->private_value;
	unsigned int value = ucontrol->value.enumerated.item[0];
	int ret = 0;

	if (value >= esetting_count(chip, id))
		return -EINVAL;

	mutex_lock(&chip->lock);
	if (chip->engine->settings.value[id] == value) {
		mutex_unlock(&chip->lock);
		return 0;
	}
	echip_log(chip, "%s: %s", ozzy_setting_descs[id].name, ozzy_setting_descs[id].value_names[value]);
	ozzy_engine_set_setting(chip->engine, id, (uint8_t)value);
	if (ozzy_setting_descs[id].kind == OZZY_SETTING_DEVICE && chip->running)
		ret = echip_restart(chip);
	mutex_unlock(&chip->lock);

	if (chip->clock_lock_kctl)
		snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE, &chip->clock_lock_kctl->id);
	return ret < 0 ? ret : 1;
}

static int eclock_lock_get(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol)
{
	struct ozzy_echip *chip = snd_kcontrol_chip(kctl);

	ucontrol->value.integer.value[0] = chip->shm->audio.settings.clock_locked;
	return 0;
}

static const struct snd_kcontrol_new eclock_lock_ctl = {
	.iface  = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name   = "S/PDIF Clock Validity",
	.access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
	.info   = snd_ctl_boolean_mono_info,
	.get    = eclock_lock_get,
};

int econtrols_init(struct ozzy_echip *chip)
{
	struct snd_kcontrol_new tmpl = {
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.info  = esetting_info,
		.get   = esetting_get,
		.put   = esetting_put,
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
		kctl = snd_ctl_new1(&eclock_lock_ctl, chip);
		ret = snd_ctl_add(chip->card, kctl);
		if (ret < 0)
			return ret;
		chip->clock_lock_kctl = kctl;
	}
	return 0;
}
