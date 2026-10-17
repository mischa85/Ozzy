// SPDX-License-Identifier: MIT
/*
 * ozzyctl backend for Linux: the snd-usb-ozzy cards through alsa-lib. Each setting is a mixer
 * control named after ozzy_setting_descs[].name (linux/ozzy_controls.c), the digital lock is
 * "S/PDIF Clock Validity". The channel counts come from the device table, through the card's
 * USB IDs in sysfs; the rate from /proc/asound while a stream is open.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include <alsa/asoundlib.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ozzyctl.h"
#include "../common/ozzy_devices.h"

#ifndef OZZY_ALSA_DRIVER
#define OZZY_ALSA_DRIVER "snd-usb-ozzy"   /* card->driver, linux/ozzy_core.c */
#endif

/* A hex number from a sysfs file, or -1 */
static long sysfs_hex(const char *dir, const char *file)
{
	char path[PATH_MAX + 32];
	long v = -1;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, file);
	f = fopen(path, "r");
	if (f) {
		if (fscanf(f, "%lx", &v) != 1)
			v = -1;
		fclose(f);
	}
	return v;
}

/* The device table row of card @card: its device link is the USB interface, whose parent
 * directory is the USB device with idVendor / idProduct */
static const struct ozzy_device *card_model(int card)
{
	char link[64], path[PATH_MAX];
	long vid, pid;

	snprintf(link, sizeof(link), "/sys/class/sound/card%d/device/..", card);
	if (!realpath(link, path))
		return NULL;
	vid = sysfs_hex(path, "idVendor");
	pid = sysfs_hex(path, "idProduct");
	return (vid < 0 || pid < 0) ? NULL : ozzy_device_lookup((uint16_t)vid, (uint16_t)pid);
}

/* The rate of an open stream of card @card (playback, else capture), or 0 */
static uint32_t card_rate(int card)
{
	static const char *const dirs[] = { "pcm0p", "pcm0c" };
	unsigned int i, rate;
	char path[96], line[128];

	for (i = 0; i < 2; i++) {
		FILE *f;

		snprintf(path, sizeof(path), "/proc/asound/card%d/%s/sub0/hw_params", card, dirs[i]);
		f = fopen(path, "r");
		if (!f)
			continue;
		while (fgets(line, sizeof(line), f)) {
			if (sscanf(line, "rate: %u", &rate) == 1) {
				fclose(f);
				return rate;
			}
		}
		fclose(f);
	}
	return 0;
}

/* Read mixer control @name: its first value. False if the card doesn't have it. */
static bool read_control(snd_ctl_t *ctl, const char *name, long *value)
{
	snd_ctl_elem_value_t *v;
	snd_ctl_elem_info_t *info;
	snd_ctl_elem_type_t type;

	snd_ctl_elem_info_alloca(&info);
	snd_ctl_elem_value_alloca(&v);
	snd_ctl_elem_info_set_interface(info, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_info_set_name(info, name);
	if (snd_ctl_elem_info(ctl, info) < 0)
		return false;
	type = snd_ctl_elem_info_get_type(info);
	snd_ctl_elem_value_set_interface(v, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_value_set_name(v, name);
	if (snd_ctl_elem_read(ctl, v) < 0)
		return false;
	if (type == SND_CTL_ELEM_TYPE_ENUMERATED)
		*value = snd_ctl_elem_value_get_enumerated(v, 0);
	else
		*value = snd_ctl_elem_value_get_integer(v, 0);
	return true;
}

int ozzyctl_backend_list(struct ozzyctl_device *devs, int max)
{
	int card = -1, n = 0;

	while (n < max && snd_card_next(&card) >= 0 && card >= 0) {
		struct ozzyctl_device *d = &devs[n];
		const struct ozzy_device *model;
		snd_ctl_card_info_t *ci;
		char hw[16];
		snd_ctl_t *ctl;
		unsigned int id;
		long v;

		snprintf(hw, sizeof(hw), "hw:%d", card);
		if (snd_ctl_open(&ctl, hw, 0) < 0)
			continue;
		snd_ctl_card_info_alloca(&ci);
		if (snd_ctl_card_info(ctl, ci) < 0 || strcmp(snd_ctl_card_info_get_driver(ci), OZZY_ALSA_DRIVER)) {
			snd_ctl_close(ctl);
			continue;
		}

		memset(d, 0, sizeof(*d));
		snprintf(d->name, sizeof(d->name), "%s", snd_ctl_card_info_get_name(ci));
		snprintf(d->id, sizeof(d->id), "%s", snd_ctl_card_info_get_id(ci));
		d->handle = (uintptr_t)card;
		d->sample_rate = card_rate(card);
		model = card_model(card);
		if (model) {
			d->in_channels = model->in_channels;
			d->out_channels = model->out_channels;
		}

		ozzy_settings_defaults(&d->settings);
		for (id = 0; id < OZZY_NUM_SETTINGS; id++) {
			if (read_control(ctl, ozzy_setting_descs[id].name, &v) && v >= 0) {
				d->available |= 1u << id;
				d->settings.value[id] = (uint8_t)v;
			}
		}
		if (read_control(ctl, "S/PDIF Clock Validity", &v)) {
			d->has_clock = true;
			d->clock.locked = v != 0;
			/* Locked, the stream runs at the lock rate (the driver allows only it) */
			d->clock.lock_rate = d->clock.locked ? d->sample_rate : 0;
		}
		snd_ctl_close(ctl);
		n++;
	}
	return n;
}

int ozzyctl_backend_set(const struct ozzyctl_device *dev, unsigned int id, unsigned int value)
{
	snd_ctl_elem_value_t *v;
	snd_ctl_t *ctl;
	char hw[16];
	int err;

	snprintf(hw, sizeof(hw), "hw:%d", (int)dev->handle);
	err = snd_ctl_open(&ctl, hw, 0);
	if (err < 0) {
		fprintf(stderr, "%s: can't open %s: %s\n", dev->name, hw, snd_strerror(err));
		return -1;
	}
	snd_ctl_elem_value_alloca(&v);
	snd_ctl_elem_value_set_interface(v, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_value_set_name(v, ozzy_setting_descs[id].name);
	snd_ctl_elem_value_set_enumerated(v, 0, value);
	/* A device setting resets the device inside this call (streams get an xrun) */
	err = snd_ctl_elem_write(ctl, v);
	snd_ctl_close(ctl);
	if (err < 0) {
		fprintf(stderr, "%s: setting %s failed: %s\n", dev->name, ozzy_setting_descs[id].name,
			snd_strerror(err));
		return -1;
	}
	return 0;
}
