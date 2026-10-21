// SPDX-License-Identifier: MIT
/*
 * The control backend for Linux: the snd-usb-ozzy cards through alsa-lib. Each setting is a mixer
 * control named after ozzy_setting_descs[].name (linux/controls.c), the digital lock is
 * "S/PDIF Clock Validity". The channel counts come from the device table, through the card's
 * USB IDs in sysfs; the driver's record (struct ozzy_info) from the bytes control "Ozzy Info";
 * the rate, period, buffer and format from /proc/asound while a stream is open, and who has it
 * open (a sound server or an app on ALSA itself). Changes come from ALSA
 * control events on each card (a setting a client set, the clock lock after each handshake) and
 * inotify on /dev/snd (cards coming and going), on one thread waiting in poll().
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include <alsa/asoundlib.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#include "ozzy_control.h"
#include "../../common/ozzy_devices.h"

#ifndef OZZY_ALSA_DRIVER
#define OZZY_ALSA_DRIVER "snd-usb-ozzy"   /* card->driver, linux/core.c */
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

/*
 * Who has the stream of card @card in @dir ("pcm0p", "pcm0c") open, into @out: ALSA's owner_pid
 * in its status, and that process's name. A sound server (PipeWire, PulseAudio, JACK) mixes the
 * apps; anything else is one app on the device itself. Empty while closed.
 */
static void stream_client(int card, const char *dir, char *out, size_t size)
{
	static const struct { const char *comm; const char *name; } servers[] = {
		{ "pipewire", "PipeWire" }, { "pulseaudio", "PulseAudio" }, { "jackd", "JACK" }, { "jackdbus", "JACK" },
	};
	char path[96], line[128], comm[64] = "";
	unsigned int pid = 0, i;
	FILE *f;

	out[0] = 0;
	snprintf(path, sizeof(path), "/proc/asound/card%d/%s/sub0/status", card, dir);
	f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f))
		sscanf(line, "owner_pid : %u", &pid);
	fclose(f);
	if (!pid)
		return;   /* "closed" */
	snprintf(path, sizeof(path), "/proc/%u/comm", pid);
	f = fopen(path, "r");
	if (f) {
		if (fgets(comm, sizeof(comm), f))
			comm[strcspn(comm, "\n")] = 0;
		fclose(f);
	}
	for (i = 0; i < sizeof(servers) / sizeof(servers[0]); i++)
		if (!strcmp(comm, servers[i].comm))
			break;
	if (i < sizeof(servers) / sizeof(servers[0]))
		snprintf(out, size, "%s (pid %u), mixing the apps", servers[i].name, pid);
	else
		snprintf(out, size, "%s (pid %u), ALSA directly", comm[0] ? comm : "an app", pid);
}

/* An open stream of card @card (playback, else capture): its rate into @rate (else 0), its
 * period, buffer and format into @h ("closed" in hw_params: none open), and who has each
 * direction open */
static void card_stream(int card, uint32_t *rate, struct ozzy_control_host *h)
{
	static const char *const dirs[] = { "pcm0p", "pcm0c" };
	unsigned int i, v;
	char path[96], line[128], word[32];

	*rate = 0;
	for (i = 0; i < 2 && !*rate; i++) {
		FILE *f;

		snprintf(path, sizeof(path), "/proc/asound/card%d/%s/sub0/hw_params", card, dirs[i]);
		f = fopen(path, "r");
		if (!f)
			continue;
		while (fgets(line, sizeof(line), f)) {
			if (sscanf(line, "rate: %u", &v) == 1)
				*rate = v;
			else if (sscanf(line, "period_size: %u", &v) == 1)
				h->buffer_frames = v;
			else if (sscanf(line, "buffer_size: %u", &v) == 1)
				h->ring_frames = v;
			else if (sscanf(line, "format: %31s", word) == 1)
				snprintf(h->format, sizeof(h->format), "%s", word);
		}
		fclose(f);
	}
	h->running = *rate != 0;
	for (i = 0; i < 2; i++)
		stream_client(card, dirs[i], h->client[i], sizeof(h->client[i]));
}

/* The bytes of card control @name into @out, at most @size; how many, or 0 if it has none */
static size_t read_bytes(snd_ctl_t *ctl, const char *name, void *out, size_t size)
{
	snd_ctl_elem_value_t *v;
	snd_ctl_elem_info_t *info;
	size_t count;

	snd_ctl_elem_info_alloca(&info);
	snd_ctl_elem_value_alloca(&v);
	snd_ctl_elem_info_set_interface(info, SND_CTL_ELEM_IFACE_CARD);   /* a card control, not a mixer one */
	snd_ctl_elem_info_set_name(info, name);
	if (snd_ctl_elem_info(ctl, info) < 0 || snd_ctl_elem_info_get_type(info) != SND_CTL_ELEM_TYPE_BYTES)
		return 0;
	snd_ctl_elem_value_set_interface(v, SND_CTL_ELEM_IFACE_CARD);
	snd_ctl_elem_value_set_name(v, name);
	if (snd_ctl_elem_read(ctl, v) < 0)
		return 0;
	count = snd_ctl_elem_info_get_count(info);
	if (count > size)
		count = size;
	memcpy(out, snd_ctl_elem_value_get_bytes(v), count);
	return count;
}

/* The driver's record from "Ozzy Info" and its stats from "Ozzy Stats", if it has ones this build reads */
static void read_info(snd_ctl_t *ctl, struct ozzy_control_device *d)
{
	struct ozzy_info info;
	struct ozzy_stats stats;
	size_t n;

	n = read_bytes(ctl, "Ozzy Info", &info, sizeof(info));
	d->has_info = n && ozzy_control_take_info(&d->info, &info, n);
	n = read_bytes(ctl, "Ozzy Stats", &stats, sizeof(stats));
	d->has_stats = n && ozzy_control_take_stats(&d->stats, &stats, n);
}

/* Whether the card behind @ctl is one of ours; its info into @ci */
static bool is_ozzy(snd_ctl_t *ctl, snd_ctl_card_info_t *ci)
{
	return snd_ctl_card_info(ctl, ci) >= 0 && !strcmp(snd_ctl_card_info_get_driver(ci), OZZY_ALSA_DRIVER);
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

int ozzy_control_backend_list(struct ozzy_control_device *devs, int max)
{
	int card = -1, n = 0;

	while (n < max && snd_card_next(&card) >= 0 && card >= 0) {
		struct ozzy_control_device *d = &devs[n];
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
		if (!is_ozzy(ctl, ci)) {
			snd_ctl_close(ctl);
			continue;
		}

		memset(d, 0, sizeof(*d));
		snprintf(d->name, sizeof(d->name), "%s", snd_ctl_card_info_get_name(ci));
		snprintf(d->id, sizeof(d->id), "%s", snd_ctl_card_info_get_id(ci));
		d->handle = (uintptr_t)card;
		snprintf(d->host.driver, sizeof(d->host.driver), "%s", OZZY_ALSA_DRIVER);
		card_stream(card, &d->sample_rate, &d->host);
		read_info(ctl, d);
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
		/* The engine runs with no stream open too: its rate, from the record */
		if (!d->sample_rate && d->has_info)
			d->sample_rate = d->info.sample_rate;
		if (read_control(ctl, "S/PDIF Clock Validity", &v)) {
			d->has_clock = true;
			d->clock.locked = v != 0;
			/* Locked, the stream runs at the lock rate (the driver allows only it) */
			d->clock.lock_rate = !d->clock.locked ? 0 : d->has_info ? d->info.lock_rate : d->sample_rate;
		}
		snd_ctl_close(ctl);
		n++;
	}
	return n;
}

int ozzy_control_backend_set(const struct ozzy_control_device *dev, unsigned int id, unsigned int value)
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

/* The watch (ozzy_control_backend_watch): one thread, waiting on inotify and each card's events */
struct watch {
	void (*changed)(void *ctx);
	void *ctx;
	int inotify;
	snd_ctl_t *ctls[OZZY_CONTROL_MAX_DEVICES];
	int nctls;
};

/* Event-subscribed control handles of our cards, after closing the previous ones */
static void watch_open(struct watch *w)
{
	int card = -1, i;

	for (i = 0; i < w->nctls; i++)
		snd_ctl_close(w->ctls[i]);
	w->nctls = 0;
	while (w->nctls < OZZY_CONTROL_MAX_DEVICES && snd_card_next(&card) >= 0 && card >= 0) {
		snd_ctl_card_info_t *ci;
		snd_ctl_t *ctl;
		char hw[16];

		snprintf(hw, sizeof(hw), "hw:%d", card);
		if (snd_ctl_open(&ctl, hw, SND_CTL_NONBLOCK) < 0)
			continue;
		snd_ctl_card_info_alloca(&ci);
		if (!is_ozzy(ctl, ci) || snd_ctl_subscribe_events(ctl, 1) < 0) {
			snd_ctl_close(ctl);
			continue;
		}
		w->ctls[w->nctls++] = ctl;
	}
}

/* Whether the inotify events waiting are about a card's control device (controlC<n>) */
static bool watch_read_inotify(int fd)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	bool cards = false;
	ssize_t len;

	while ((len = read(fd, buf, sizeof(buf))) > 0) {
		char *p;

		for (p = buf; p < buf + len; p += sizeof(struct inotify_event) + ((struct inotify_event *)p)->len) {
			const struct inotify_event *ev = (const struct inotify_event *)p;

			if (ev->len && !strncmp(ev->name, "controlC", 8))
				cards = true;
			if (ev->mask & IN_Q_OVERFLOW)
				cards = true;
		}
	}
	return cards;
}

static void *watch_thread(void *arg)
{
	struct watch *w = arg;
	struct pollfd fds[1 + OZZY_CONTROL_MAX_DEVICES * 4];
	int owner[1 + OZZY_CONTROL_MAX_DEVICES * 4];

	for (;;) {
		bool reopen = false, changed = false;
		int nfds = 1, i, k;

		fds[0].fd = w->inotify;
		fds[0].events = POLLIN;
		owner[0] = -1;
		for (i = 0; i < w->nctls; i++) {
			int n = snd_ctl_poll_descriptors(w->ctls[i], &fds[nfds], (unsigned int)(sizeof(fds) / sizeof(fds[0]) - nfds));

			for (k = 0; k < n; k++)
				owner[nfds + k] = i;
			nfds += n > 0 ? n : 0;
		}
		if (poll(fds, (nfds_t)nfds, -1) < 0) {
			if (errno == EINTR)
				continue;
			perror("watch: poll");   /* not going to get better: stop rather than spin */
			return NULL;
		}

		if (fds[0].revents & POLLIN)
			reopen = watch_read_inotify(w->inotify);
		for (k = 1; k < nfds; k++) {
			snd_ctl_event_t *ev;

			if (!fds[k].revents)
				continue;
			if (fds[k].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				reopen = true;   /* the card is going away */
				continue;
			}
			snd_ctl_event_alloca(&ev);
			while (snd_ctl_read(w->ctls[owner[k]], ev) > 0)
				changed = true;
		}
		if (reopen)
			watch_open(w);
		if (reopen || changed)
			w->changed(w->ctx);
	}
	return NULL;
}

int ozzy_control_backend_watch(void (*changed)(void *ctx), void *ctx)
{
	static struct watch w;
	pthread_t thread;

	w.changed = changed;
	w.ctx = ctx;
	/* A card's control device appears and goes; udev then gives it its group (IN_ATTRIB) */
	w.inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (w.inotify < 0 || inotify_add_watch(w.inotify, "/dev/snd", IN_CREATE | IN_DELETE | IN_ATTRIB) < 0) {
		perror("inotify on /dev/snd");
		return -1;
	}
	/* Subscribed before returning: a change right after is heard */
	watch_open(&w);
	if (pthread_create(&thread, NULL, watch_thread, &w)) {
		fprintf(stderr, "can't start the watch thread\n");
		return -1;
	}
	pthread_detach(thread);
	return 0;
}
