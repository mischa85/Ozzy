// SPDX-License-Identifier: MIT
/*
 * ozzy-cli: the console tool, on the shared control core and a backend (control/ozzy_control.h)
 *
 *   ozzy-cli [list]                             devices
 *   ozzy-cli show [-d DEVICE]                   a device's settings, and all the driver knows
 *   ozzy-cli set [-d DEVICE] SETTING VALUE ...  change one or more settings
 *   ozzy-cli watch                              every device, again whenever it changes
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

#include "../control/ozzy_control.h"

/* How long "set" waits for the next change while the setting hasn't read back (a device
 * setting restarts streaming, about a second) */
#define OZZY_CLI_SET_WAIT_MS 5000
#define OZZY_CLI_FOREVER 0xffffffffu

/*
 * The backend's change notifications (ozzy_control_backend_watch) as a count the main thread waits
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

/* Wait until there were changes after the count @seen, at most @ms (OZZY_CLI_FOREVER: no limit).
 * False if none came. */
static bool wait_change(unsigned long seen, unsigned int ms)
{
	bool changed;

#ifdef _WIN32
	AcquireSRWLockExclusive(&change_lock);
	while (changes == seen &&
	       SleepConditionVariableSRW(&change_cond, &change_lock, ms == OZZY_CLI_FOREVER ? INFINITE : ms, 0))
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
	while (changes == seen && !err)   /* ETIMEDOUT, or an error: give up */
		err = ms == OZZY_CLI_FOREVER ? pthread_cond_wait(&change_cond, &change_lock)
					    : pthread_cond_timedwait(&change_cond, &change_lock, &until);
	changed = changes != seen;
	pthread_mutex_unlock(&change_lock);
#endif
	return changed;
}

/* Milliseconds on a clock that only goes forward */
static uint64_t now_ms(void)
{
#ifdef _WIN32
	return GetTickCount64();
#else
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
#endif
}

static void print_value(const struct ozzy_control_device *dev, unsigned int id)
{
	const struct ozzy_setting_desc *d = &ozzy_setting_descs[id];
	unsigned int v = dev->settings.value[id], n = ozzy_control_values(dev, id), i;
	char s[96];

	ozzy_control_slug(d->name, s, sizeof(s));
	printf("  %-24s ", s);
	ozzy_control_slug(v < n ? d->value_names[v] : "?", s, sizeof(s));
	printf("%-16s (", s);
	for (i = 0; i < n; i++) {
		ozzy_control_slug(d->value_names[i], s, sizeof(s));
		printf("%s%s", i ? ", " : "", s);
	}
	printf(")%s\n", d->kind == OZZY_SETTING_DEVICE ? "" : "  [host]");
}

/* The settings and their choices */
static void show_settings(const struct ozzy_control_device *dev)
{
	unsigned int id, n = 0;

	printf("Settings\n");
	/* A setting without values (channel counts unknown) can't be set: left out */
	for (id = 0; id < OZZY_NUM_SETTINGS; id++) {
		if (ozzy_control_values(dev, id)) {
			print_value(dev, id);
			n++;
		}
	}
	if (!n)
		printf("  (none)\n");
}

/* Everything known about @dev: the settings, then the facts by group (ozzy_control_facts) */
static void show(const struct ozzy_control_device *dev)
{
	struct ozzy_control_fact facts[OZZY_CONTROL_MAX_FACTS];
	int n = ozzy_control_facts(dev, facts, OZZY_CONTROL_MAX_FACTS), i;

	printf("%s\n", dev->name);
	show_settings(dev);
	for (i = 0; i < n; i++) {
		if (!i || facts[i].group != facts[i - 1].group)
			printf("%s\n", ozzy_control_group_names[facts[i].group]);
		printf("  %-24s %s\n", facts[i].label, facts[i].value);
	}
	if (!dev->has_info)
		printf("(The driver gives no more: it is older than these tools.)\n");
}

static void list(const struct ozzy_control_device *devs, int n)
{
	int i, k;

	if (!n) {
		printf("No Ozzy devices found.\n");
		return;
	}
	for (i = 0; i < n; i++) {
		unsigned int count = 0;

		for (k = 0; k < OZZY_NUM_SETTINGS; k++)
			count += !!ozzy_control_values(&devs[i], (unsigned int)k);
		if (devs[i].sample_rate)
			printf("%d  %-32s %6u Hz  %u setting%s\n", i, devs[i].name, devs[i].sample_rate,
			       count, count == 1 ? "" : "s");
		else
			printf("%d  %-32s %9s  %u setting%s\n", i, devs[i].name, "-", count,
			       count == 1 ? "" : "s");
	}
}

static int set(const struct ozzy_control_device *dev, int argc, char **argv)
{
	unsigned int ids[OZZY_NUM_SETTINGS], vals[OZZY_NUM_SETTINGS], count = 0, i;
	struct ozzy_control_device now;

	if (argc == 0 || argc % 2) {
		fprintf(stderr, "set: give SETTING VALUE pairs\n");
		return 2;
	}
	for (; argc >= 2 && count < OZZY_NUM_SETTINGS; argc -= 2, argv += 2) {
		int id = ozzy_control_find_setting(dev, argv[0]), v;

		if (id < 0) {
			fprintf(stderr, "%s has no setting \"%s\" (see: ozzy-cli show)\n", dev->name, argv[0]);
			return 1;
		}
		v = ozzy_control_find_value(dev, (unsigned int)id, argv[1]);
		if (v < 0) {
			fprintf(stderr, "\"%s\" is not a value of %s (see: ozzy-cli show)\n", argv[1],
				ozzy_setting_descs[id].name);
			return 1;
		}
		ids[count] = (unsigned int)id;
		vals[count++] = (unsigned int)v;
	}

	if (ozzy_control_backend_watch(on_change, NULL) < 0)
		return 1;
	for (i = 0; i < count; i++)
		if (ozzy_control_backend_set(dev, ids[i], vals[i]) < 0)
			return 1;

	/* A device setting restarts the device's streaming: wait until it reads back, at most
	 * OZZY_CLI_SET_WAIT_MS however many other changes come meanwhile */
	for (uint64_t deadline = now_ms() + OZZY_CLI_SET_WAIT_MS;;) {
		unsigned long seen = changes_now();
		bool done = ozzy_control_reread(dev->id, &now);
		uint64_t t = now_ms();

		for (i = 0; done && i < count; i++)
			done = now.settings.value[ids[i]] == vals[i];
		if (done)
			break;
		if (t >= deadline || !wait_change(seen, (unsigned int)(deadline - t))) {
			fprintf(stderr, "the device didn't report the new value within %u s\n",
				OZZY_CLI_SET_WAIT_MS / 1000);
			return 1;
		}
	}
	printf("%s\n", now.name);
	show_settings(&now);
	return 0;
}

/* Every device, then each one again when it changes, arrives or leaves; until interrupted */
static int watch(void)
{
	struct ozzy_control_device last[OZZY_CONTROL_MAX_DEVICES], now[OZZY_CONTROL_MAX_DEVICES];
	unsigned long seen;
	int nlast, nnow, i, k;

	/* Watching first, so a change while listing isn't missed */
	if (ozzy_control_backend_watch(on_change, NULL) < 0)
		return 1;
	seen = changes_now();
	nlast = ozzy_control_backend_list(last, OZZY_CONTROL_MAX_DEVICES);
	if (nlast < 0)
		return 1;
	if (!nlast)
		printf("No Ozzy devices found; waiting for one.\n");
	for (i = 0; i < nlast; i++)
		show(&last[i]);
	fflush(stdout);

	for (;;) {
		wait_change(seen, OZZY_CLI_FOREVER);
		seen = changes_now();
		nnow = ozzy_control_backend_list(now, OZZY_CONTROL_MAX_DEVICES);
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
			if (i == nlast || !ozzy_control_same(&now[k], &last[i]))
				show(&now[k]);
		}
		fflush(stdout);
		memcpy(last, now, sizeof(now[0]) * (size_t)nnow);
		nlast = nnow;
	}
}

static void usage(void)
{
	printf("usage: ozzy-cli [list]\n"
	       "       ozzy-cli show [-d DEVICE]\n"
	       "       ozzy-cli set [-d DEVICE] SETTING VALUE [SETTING VALUE ...]\n"
	       "       ozzy-cli watch\n"
	       "DEVICE: a number from \"list\" or part of the name (default: the first).\n"
	       "SETTING, VALUE: names as \"show\" prints them, or numbers.\n"
	       "[host]: applied by the driver on the computer, at once; the others restart streaming.\n"
	       "watch: every device, then again whenever one changes, arrives or leaves (Ctrl-C ends).\n");
}

int main(int argc, char **argv)
{
	struct ozzy_control_device devs[OZZY_CONTROL_MAX_DEVICES];
	const struct ozzy_control_device *dev;
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
	n = ozzy_control_backend_list(devs, OZZY_CONTROL_MAX_DEVICES);
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
	dev = ozzy_control_pick(devs, n, which);
	if (!dev) {
		fprintf(stderr, n ? "No device \"%s\" (see: ozzy-cli list)\n" : "No Ozzy devices found.\n",
			which ? which : "");
		return 1;
	}
	if (!strcmp(cmd, "show")) {
		show(dev);
		return 0;
	}
	return set(dev, argc, argv);
}
