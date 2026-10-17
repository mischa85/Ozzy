// SPDX-License-Identifier: MIT
/*
 * ozzyctl: the platform-neutral core (see ozzyctl.h)
 *
 *   ozzyctl [list]                              devices
 *   ozzyctl show [-d DEVICE]                    a device's settings and their choices
 *   ozzyctl set [-d DEVICE] SETTING VALUE ...   change one or more settings
 *
 * DEVICE is the number from "list" or part of the name (default: the first device).
 * SETTING and VALUE are names as "show" prints them (lower case, dashes: clock-source,
 * internal), case-insensitive, or numbers.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ozzyctl.h"

/* How long "set" waits for a device setting to read back changed (it restarts streaming) */
#define OZZYCTL_SET_WAIT_MS 5000

/*
 * slug - @name in lower case with runs of other characters as one dash ("S/PDIF" -> "s-pdif").
 */
static void slug(const char *name, char *out, size_t size)
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

	slug(arg, a, sizeof(a));
	slug(name, b, sizeof(b));
	return strcmp(a, b) == 0;
}

/* @arg as a non-negative number, or -1 */
static long number(const char *arg)
{
	char *end;
	long v = strtol(arg, &end, 10);

	return (*arg && !*end && v >= 0) ? v : -1;
}

static unsigned int values_of(const struct ozzyctl_device *dev, unsigned int id)
{
	return (dev->available & (1u << id)) ? ozzy_setting_count(id, dev->in_channels, dev->out_channels) : 0;
}

static void print_value(const struct ozzyctl_device *dev, unsigned int id)
{
	const struct ozzy_setting_desc *d = &ozzy_setting_descs[id];
	unsigned int v = dev->settings.value[id], n = values_of(dev, id), i;
	char s[96];

	slug(d->name, s, sizeof(s));
	printf("  %-24s ", s);
	slug(v < n ? d->value_names[v] : "?", s, sizeof(s));
	printf("%-16s (", s);
	for (i = 0; i < n; i++) {
		slug(d->value_names[i], s, sizeof(s));
		printf("%s%s", i ? ", " : "", s);
	}
	printf(")%s\n", d->kind == OZZY_SETTING_DEVICE ? "" : "  [host]");
}

static void show(const struct ozzyctl_device *dev)
{
	unsigned int id;

	printf("%s\n", dev->name);
	if (dev->sample_rate)
		printf("  %-24s %u Hz, %u in / %u out\n", "running at", dev->sample_rate,
		       dev->in_channels, dev->out_channels);
	else
		printf("  %-24s %u in / %u out (rate: not streaming)\n", "channels", dev->in_channels,
		       dev->out_channels);
	if (dev->has_clock) {
		if (dev->clock.locked && dev->clock.lock_rate)
			printf("  %-24s locked to S/PDIF at %u Hz\n", "clock", dev->clock.lock_rate);
		else if (dev->clock.locked)
			printf("  %-24s locked to S/PDIF\n", "clock");
		else
			printf("  %-24s internal\n", "clock");
	}
	if (!dev->available) {
		printf("  (no settings)\n");
		return;
	}
	/* A setting without values (channel counts unknown) can't be set: left out */
	for (id = 0; id < OZZY_NUM_SETTINGS; id++)
		if (values_of(dev, id))
			print_value(dev, id);
}

static void list(const struct ozzyctl_device *devs, int n)
{
	int i, k;

	if (!n) {
		printf("No Ozzy devices found.\n");
		return;
	}
	for (i = 0; i < n; i++) {
		unsigned int count = 0;

		for (k = 0; k < OZZY_NUM_SETTINGS; k++)
			count += !!values_of(&devs[i], (unsigned int)k);
		if (devs[i].sample_rate)
			printf("%d  %-32s %6u Hz  %u setting%s\n", i, devs[i].name, devs[i].sample_rate,
			       count, count == 1 ? "" : "s");
		else
			printf("%d  %-32s %9s  %u setting%s\n", i, devs[i].name, "-", count,
			       count == 1 ? "" : "s");
	}
}

/* The device @arg picks (NULL: the first), or NULL */
static const struct ozzyctl_device *pick(const struct ozzyctl_device *devs, int n, const char *arg)
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
	slug(arg, a, sizeof(a));
	for (i = 0; i < n; i++) {
		slug(devs[i].name, b, sizeof(b));
		if (strstr(b, a))
			return &devs[i];
	}
	return NULL;
}

/* The device with @id in a fresh list, into @out; false if gone */
static bool reread(const char *id, struct ozzyctl_device *out)
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

static int set(const struct ozzyctl_device *dev, int argc, char **argv)
{
	unsigned int ids[OZZY_NUM_SETTINGS], vals[OZZY_NUM_SETTINGS], count = 0, i, waited;
	struct ozzyctl_device now;

	if (argc == 0 || argc % 2) {
		fprintf(stderr, "set: give SETTING VALUE pairs\n");
		return 2;
	}
	for (; argc >= 2 && count < OZZY_NUM_SETTINGS; argc -= 2, argv += 2) {
		long id = number(argv[0]), v;
		unsigned int k, n;

		for (k = 0; id < 0 && k < OZZY_NUM_SETTINGS; k++)
			if (names(argv[0], ozzy_setting_descs[k].name))
				id = k;
		if (id < 0 || id >= OZZY_NUM_SETTINGS || !values_of(dev, (unsigned int)id)) {
			fprintf(stderr, "%s has no setting \"%s\" (see: ozzyctl show)\n", dev->name, argv[0]);
			return 1;
		}
		n = values_of(dev, (unsigned int)id);
		v = number(argv[1]);
		for (k = 0; v < 0 && k < n; k++)
			if (names(argv[1], ozzy_setting_descs[id].value_names[k]))
				v = k;
		if (v < 0 || (unsigned long)v >= n) {
			fprintf(stderr, "\"%s\" is not a value of %s (see: ozzyctl show)\n", argv[1],
				ozzy_setting_descs[id].name);
			return 1;
		}
		ids[count] = (unsigned int)id;
		vals[count++] = (unsigned int)v;
	}

	for (i = 0; i < count; i++)
		if (ozzyctl_backend_set(dev, ids[i], vals[i]) < 0)
			return 1;

	/* A device setting restarts the device's streaming: wait until it reads back */
	for (waited = 0;; waited += 100) {
		bool done = reread(dev->id, &now);

		for (i = 0; done && i < count; i++)
			done = now.settings.value[ids[i]] == vals[i];
		if (done)
			break;
		if (waited >= OZZYCTL_SET_WAIT_MS) {
			fprintf(stderr, "the device didn't report the new value within %u s\n",
				OZZYCTL_SET_WAIT_MS / 1000);
			return 1;
		}
		usleep(100 * 1000);
	}
	show(&now);
	return 0;
}

static void usage(void)
{
	printf("usage: ozzyctl [list]\n"
	       "       ozzyctl show [-d DEVICE]\n"
	       "       ozzyctl set [-d DEVICE] SETTING VALUE [SETTING VALUE ...]\n"
	       "DEVICE: a number from \"list\" or part of the name (default: the first).\n"
	       "SETTING, VALUE: names as \"show\" prints them, or numbers.\n"
	       "[host]: applied by the driver on the computer, at once; the others restart streaming.\n");
}

int main(int argc, char **argv)
{
	struct ozzyctl_device devs[OZZYCTL_MAX_DEVICES];
	const struct ozzyctl_device *dev;
	const char *cmd = argc > 1 ? argv[1] : "list";
	const char *which = NULL;
	int n;

	if (!strcmp(cmd, "help") || !strcmp(cmd, "-h") || !strcmp(cmd, "--help")) {
		usage();
		return 0;
	}
	argc -= 2;
	argv += 2;
	if (argc >= 2 && !strcmp(argv[0], "-d")) {
		which = argv[1];
		argc -= 2;
		argv += 2;
	}

	n = ozzyctl_backend_list(devs, OZZYCTL_MAX_DEVICES);
	if (n < 0)
		return 1;
	if (!strcmp(cmd, "list")) {
		list(devs, n);
		return 0;
	}
	if (strcmp(cmd, "show") && strcmp(cmd, "set")) {
		usage();
		return 2;
	}
	dev = pick(devs, n, which);
	if (!dev) {
		fprintf(stderr, n ? "No device \"%s\" (see: ozzyctl list)\n" : "No Ozzy devices found.\n",
			which ? which : "");
		return 1;
	}
	if (!strcmp(cmd, "show")) {
		show(dev);
		return 0;
	}
	return set(dev, argc, argv);
}
