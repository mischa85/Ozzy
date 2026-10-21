// SPDX-License-Identifier: MIT
/*
 * Ozzy USB Audio Driver - device settings as ALSA controls on the engine
 *
 * One enumerated control per setting the device has, named as common/ozzy_settings.h names
 * it (apps/control/backend_linux.c finds them by name), plus "S/PDIF Clock Validity" with the clock
 * source, "Ozzy Info": the engine's struct ozzy_info as bytes, for ozzy-cli and the ozzy app, and
 * "Ozzy Stats": its struct ozzy_stats, live.
 * A route applies at once (the engine routes from the next packet); a device setting
 * restarts the engine, whose handshake sends it.
 *
 * Clients learn of changes from control events (snd_ctl_subscribe_events): ALSA sends one for
 * a setting a client changed, the driver one for the clock lock and the info after each
 * handshake (ozzy_controls_handshake), the only time the engine learns them, and one for the
 * stats after an incident (ozzy_controls_stats_work).
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
	return ret < 0 ? ret : 1;
}

/* After a handshake (the engine started, or failed to): the clock lock and the info may have
 * changed */
void ozzy_controls_handshake(struct ozzy_chip *chip)
{
	if (chip->clock_lock_kctl)
		snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE, &chip->clock_lock_kctl->id);
	ozzy_controls_stream_changed(chip);
}

/* A stream opened, was set up or closed: clients read its rate, period and buffer again (they
 * come from /proc, which has no events of its own); the info control stands for them */
void ozzy_controls_stream_changed(struct ozzy_chip *chip)
{
	if (chip->info_kctl)
		snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE, &chip->info_kctl->id);
}

static int info_info(struct snd_kcontrol *kctl, struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BYTES;
	uinfo->count = sizeof(struct ozzy_info);
	return 0;
}

/* Under the lock: a restart writes it */
static int info_get(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol)
{
	struct ozzy_chip *chip = snd_kcontrol_chip(kctl);

	mutex_lock(&chip->lock);
	memcpy(ucontrol->value.bytes.data, &chip->shm->info, sizeof(struct ozzy_info));
	mutex_unlock(&chip->lock);
	return 0;
}

/* A card control, not a mixer one: mixers and alsactl store leave the blob alone */
static const struct snd_kcontrol_new info_ctl = {
	.iface  = SNDRV_CTL_ELEM_IFACE_CARD,
	.name   = "Ozzy Info",
	.access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
	.info   = info_info,
	.get    = info_get,
};

/* The bytes control carries the whole record */
static_assert(sizeof(struct ozzy_info) <= sizeof(((struct snd_ctl_elem_value *)0)->value.bytes.data));

static int stats_info(struct snd_kcontrol *kctl, struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BYTES;
	uinfo->count = sizeof(struct ozzy_stats);
	return 0;
}

/* Live: the completions write it as they go (one writer a field, ozzy_stats.h) */
static int stats_get(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol)
{
	struct ozzy_chip *chip = snd_kcontrol_chip(kctl);

	memcpy(ucontrol->value.bytes.data, &chip->shm->stats, sizeof(struct ozzy_stats));
	return 0;
}

static const struct snd_kcontrol_new stats_ctl = {
	.iface  = SNDRV_CTL_ELEM_IFACE_CARD,
	.name   = "Ozzy Stats",
	.access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
	.info   = stats_info,
	.get    = stats_get,
};

static_assert(sizeof(struct ozzy_stats) <= sizeof(((struct snd_ctl_elem_value *)0)->value.bytes.data));

/* A stats incident (the bus's stats_changed, 250 ms on) */
void ozzy_controls_stats_work(struct work_struct *work)
{
	struct ozzy_chip *chip = container_of(to_delayed_work(work), struct ozzy_chip, stats_work);

	if (chip->stats_kctl)
		snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE, &chip->stats_kctl->id);
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
	kctl = snd_ctl_new1(&info_ctl, chip);
	ret = snd_ctl_add(chip->card, kctl);
	if (ret < 0)
		return ret;
	chip->info_kctl = kctl;
	kctl = snd_ctl_new1(&stats_ctl, chip);
	ret = snd_ctl_add(chip->card, kctl);
	if (ret < 0)
		return ret;
	chip->stats_kctl = kctl;
	return 0;
}
