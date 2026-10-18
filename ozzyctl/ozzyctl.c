// SPDX-License-Identifier: MIT
/*
 * ozzyctl: the console tool, on the core and a backend (see ozzyctl.h)
 *
 *   ozzyctl [list]                              devices
 *   ozzyctl show [-d DEVICE]                    a device's settings and their choices
 *   ozzyctl set [-d DEVICE] SETTING VALUE ...   change one or more settings
 *   ozzyctl watch                               every device, again whenever it changes
 *
 * DEVICE is the number from "list" or part of the name (default: the first device).
 * SETTING and VALUE are names as "show" prints them (lower case, dashes: clock-source,
 * internal), case-insensitive, or numbers.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <pthread.h>
#include <time.h>
#endif

#include "ozzyctl.h"

/* How long "set" waits for the next change while the setting hasn't read back (a device
 * setting restarts streaming, about a second) */
#define OZZYCTL_SET_WAIT_MS 5000
#define OZZYCTL_FOREVER 0xffffffffu

/*
 * The backend's change notifications (ozzyctl_backend_watch) as a count the main thread waits
 * on: it reads the count, looks at the devices, then waits for the count to move on.
 */
#ifdef _WIN32
static SRWLOCK change_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE change_cond = CONDITION_VARIABLE_INIT;
#else
static pthread_mutex_t change_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t change_cond = PTHREAD_COND_INITIALIZER;
#endif
static unsigned long changes;

static void on_change(void *ctx)
{
	(void)ctx;
#ifdef _WIN32
	AcquireSRWLockExclusive(&change_lock);
	changes++;
	ReleaseSRWLockExclusive(&change_lock);
	WakeAllConditionVariable(&change_cond);
#else
	pthread_mutex_lock(&change_lock);
	changes++;
	pthread_cond_broadcast(&change_cond);
	pthread_mutex_unlock(&change_lock);
#endif
}

static unsigned long changes_now(void)
{
	unsigned long n;

#ifdef _WIN32
	AcquireSRWLockShared(&change_lock);
	n = changes;
	ReleaseSRWLockShared(&change_lock);
#else
	pthread_mutex_lock(&change_lock);
	n = changes;
	pthread_mutex_unlock(&change_lock);
#endif
	return n;
}

/* Wait until there were changes after the count @seen, at most @ms (OZZYCTL_FOREVER: no limit).
 * False if none came. */
static bool wait_change(unsigned long seen, unsigned int ms)
{
	bool changed;

#ifdef _WIN32
	AcquireSRWLockExclusive(&change_lock);
	while (changes == seen &&
	       SleepConditionVariableSRW(&change_cond, &change_lock, ms == OZZYCTL_FOREVER ? INFINITE : ms, 0))
		;
	changed = changes != seen;
	ReleaseSRWLockExclusive(&change_lock);
#else
	struct timespec until;
	int err = 0;

	clock_gettime(CLOCK_REALTIME, &until);
	until.tv_sec += ms / 1000;
	until.tv_nsec += (long)(ms % 1000) * 1000000;
	if (until.tv_nsec >= 1000000000) {
		until.tv_sec++;
		until.tv_nsec -= 1000000000;
	}
	pthread_mutex_lock(&change_lock);
	while (changes == seen && err != ETIMEDOUT)
		err = ms == OZZYCTL_FOREVER ? pthread_cond_wait(&change_cond, &change_lock)
					    : pthread_cond_timedwait(&change_cond, &change_lock, &until);
	changed = changes != seen;
	pthread_mutex_unlock(&change_lock);
#endif
	return changed;
}

static void print_value(const struct ozzyctl_device *dev, unsigned int id)
{
	const struct ozzy_setting_desc *d = &ozzy_setting_descs[id];
	unsigned int v = dev->settings.value[id], n = ozzyctl_values(dev, id), i;
	char s[96];

	ozzyctl_slug(d->name, s, sizeof(s));
	printf("  %-24s ", s);
	ozzyctl_slug(v < n ? d->value_names[v] : "?", s, sizeof(s));
	printf("%-16s (", s);
	for (i = 0; i < n; i++) {
		ozzyctl_slug(d->value_names[i], s, sizeof(s));
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
		if (ozzyctl_values(dev, id))
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
			count += !!ozzyctl_values(&devs[i], (unsigned int)k);
		if (devs[i].sample_rate)
			printf("%d  %-32s %6u Hz  %u setting%s\n", i, devs[i].name, devs[i].sample_rate,
			       count, count == 1 ? "" : "s");
		else
			printf("%d  %-32s %9s  %u setting%s\n", i, devs[i].name, "-", count,
			       count == 1 ? "" : "s");
	}
}

static int set(const struct ozzyctl_device *dev, int argc, char **argv)
{
	unsigned int ids[OZZY_NUM_SETTINGS], vals[OZZY_NUM_SETTINGS], count = 0, i;
	struct ozzyctl_device now;

	if (argc == 0 || argc % 2) {
		fprintf(stderr, "set: give SETTING VALUE pairs\n");
		return 2;
	}
	for (; argc >= 2 && count < OZZY_NUM_SETTINGS; argc -= 2, argv += 2) {
		int id = ozzyctl_find_setting(dev, argv[0]), v;

		if (id < 0) {
			fprintf(stderr, "%s has no setting \"%s\" (see: ozzyctl show)\n", dev->name, argv[0]);
			return 1;
		}
		v = ozzyctl_find_value(dev, (unsigned int)id, argv[1]);
		if (v < 0) {
			fprintf(stderr, "\"%s\" is not a value of %s (see: ozzyctl show)\n", argv[1],
				ozzy_setting_descs[id].name);
			return 1;
		}
		ids[count] = (unsigned int)id;
		vals[count++] = (unsigned int)v;
	}

	if (ozzyctl_backend_watch(on_change, NULL) < 0)
		return 1;
	for (i = 0; i < count; i++)
		if (ozzyctl_backend_set(dev, ids[i], vals[i]) < 0)
			return 1;

	/* A device setting restarts the device's streaming: wait until it reads back */
	for (;;) {
		unsigned long seen = changes_now();
		bool done = ozzyctl_reread(dev->id, &now);

		for (i = 0; done && i < count; i++)
			done = now.settings.value[ids[i]] == vals[i];
		if (done)
			break;
		if (!wait_change(seen, OZZYCTL_SET_WAIT_MS)) {
			fprintf(stderr, "the device didn't report the new value within %u s\n",
				OZZYCTL_SET_WAIT_MS / 1000);
			return 1;
		}
	}
	show(&now);
	return 0;
}

/* Every device, then each one again when it changes, arrives or leaves; until interrupted */
static int watch(void)
{
	struct ozzyctl_device last[OZZYCTL_MAX_DEVICES], now[OZZYCTL_MAX_DEVICES];
	unsigned long seen;
	int nlast, nnow, i, k;

	/* Watching first, so a change while listing isn't missed */
	if (ozzyctl_backend_watch(on_change, NULL) < 0)
		return 1;
	seen = changes_now();
	nlast = ozzyctl_backend_list(last, OZZYCTL_MAX_DEVICES);
	if (nlast < 0)
		return 1;
	if (!nlast)
		printf("No Ozzy devices found; waiting for one.\n");
	for (i = 0; i < nlast; i++)
		show(&last[i]);
	fflush(stdout);

	for (;;) {
		wait_change(seen, OZZYCTL_FOREVER);
		seen = changes_now();
		nnow = ozzyctl_backend_list(now, OZZYCTL_MAX_DEVICES);
		if (nnow < 0)
			continue;
		for (i = 0; i < nlast; i++) {
			for (k = 0; k < nnow && strcmp(now[k].id, last[i].id); k++)
				;
			if (k == nnow)
				printf("%s: gone\n", last[i].name);
		}
		for (k = 0; k < nnow; k++) {
			for (i = 0; i < nlast && strcmp(now[k].id, last[i].id); i++)
				;
			if (i == nlast || !ozzyctl_same(&now[k], &last[i]))
				show(&now[k]);
		}
		fflush(stdout);
		memcpy(last, now, sizeof(now[0]) * (size_t)nnow);
		nlast = nnow;
	}
}

static void usage(void)
{
	printf("usage: ozzyctl [list]\n"
	       "       ozzyctl show [-d DEVICE]\n"
	       "       ozzyctl set [-d DEVICE] SETTING VALUE [SETTING VALUE ...]\n"
	       "       ozzyctl watch\n"
	       "DEVICE: a number from \"list\" or part of the name (default: the first).\n"
	       "SETTING, VALUE: names as \"show\" prints them, or numbers.\n"
	       "[host]: applied by the driver on the computer, at once; the others restart streaming.\n"
	       "watch: every device, then again whenever one changes, arrives or leaves (Ctrl-C ends).\n");
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

	if (!strcmp(cmd, "watch"))
		return watch();
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
	dev = ozzyctl_pick(devs, n, which);
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
