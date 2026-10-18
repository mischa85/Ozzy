// SPDX-License-Identifier: MIT
/*
 * ozzyctl: the platform-neutral core every front end shares (see ozzyctl.h): setting and value
 * names, and finding devices, settings and values by what a user typed or picked.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "ozzyctl.h"

void ozzyctl_slug(const char *name, char *out, size_t size)
{
	size_t n = 0;
	bool dash = false;

	for (; *name && n + 1 < size; name++) {
		if (isalnum((unsigned char)*name)) {
			if (dash && n > 0 && n + 2 < size)
				out[n++] = '-';
			out[n++] = (char)tolower((unsigned char)*name);
			dash = false;
		} else {
			dash = true;
		}
	}
	out[n] = 0;
}

/* Whether @arg names @name: its slug, or the name itself, ignoring case */
static bool names(const char *arg, const char *name)
{
	char a[96], b[96];

	ozzyctl_slug(arg, a, sizeof(a));
	ozzyctl_slug(name, b, sizeof(b));
	return strcmp(a, b) == 0;
}

/* @arg as a non-negative number, or -1 */
static long number(const char *arg)
{
	char *end;
	long v = strtol(arg, &end, 10);

	return (*arg && !*end && v >= 0) ? v : -1;
}

unsigned int ozzyctl_values(const struct ozzyctl_device *dev, unsigned int id)
{
	return (dev->available & (1u << id)) ? ozzy_setting_count(id, dev->in_channels, dev->out_channels) : 0;
}

const struct ozzyctl_device *ozzyctl_pick(const struct ozzyctl_device *devs, int n, const char *arg)
{
	char a[96], b[96];
	long idx;
	int i;

	if (!n)
		return NULL;
	if (!arg)
		return &devs[0];
	idx = number(arg);
	if (idx >= 0)
		return idx < n ? &devs[idx] : NULL;
	ozzyctl_slug(arg, a, sizeof(a));
	for (i = 0; i < n; i++) {
		ozzyctl_slug(devs[i].name, b, sizeof(b));
		if (strstr(b, a))
			return &devs[i];
	}
	return NULL;
}

int ozzyctl_find_setting(const struct ozzyctl_device *dev, const char *arg)
{
	long id = number(arg);
	unsigned int k;

	for (k = 0; id < 0 && k < OZZY_NUM_SETTINGS; k++)
		if (names(arg, ozzy_setting_descs[k].name))
			id = k;
	return (id >= 0 && id < OZZY_NUM_SETTINGS && ozzyctl_values(dev, (unsigned int)id)) ? (int)id : -1;
}

int ozzyctl_find_value(const struct ozzyctl_device *dev, unsigned int id, const char *arg)
{
	unsigned int n = ozzyctl_values(dev, id), k;
	long v = number(arg);

	for (k = 0; v < 0 && k < n; k++)
		if (names(arg, ozzy_setting_descs[id].value_names[k]))
			v = k;
	return (v >= 0 && (unsigned long)v < n) ? (int)v : -1;
}

bool ozzyctl_reread(const char *id, struct ozzyctl_device *out)
{
	struct ozzyctl_device devs[OZZYCTL_MAX_DEVICES];
	int i, n = ozzyctl_backend_list(devs, OZZYCTL_MAX_DEVICES);

	for (i = 0; i < n; i++) {
		if (strcmp(devs[i].id, id) == 0) {
			*out = devs[i];
			return true;
		}
	}
	return false;
}

bool ozzyctl_same(const struct ozzyctl_device *a, const struct ozzyctl_device *b)
{
	unsigned int id;

	if (strcmp(a->name, b->name) || strcmp(a->id, b->id) || a->sample_rate != b->sample_rate ||
	    a->in_channels != b->in_channels || a->out_channels != b->out_channels ||
	    a->available != b->available || a->has_clock != b->has_clock)
		return false;
	if (a->has_clock && (a->clock.locked != b->clock.locked || a->clock.lock_rate != b->clock.lock_rate))
		return false;
	for (id = 0; id < OZZY_NUM_SETTINGS; id++)
		if ((a->available & (1u << id)) && a->settings.value[id] != b->settings.value[id])
			return false;
	return true;
}
