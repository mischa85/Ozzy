// SPDX-License-Identifier: MIT
/*
 * The control backend for trying a front end without hardware (gui: meson -Ddemo=true): two
 * made-up devices from the device table, a Tascam US-600 (clock source, S/PDIF out mode and
 * routing) and a Xone:DB4 (no settings). A device setting takes a moment, as a real restart does;
 * Automatic locks to a made-up S/PDIF input at 44.1 kHz. Nothing reaches a driver.
 *
 * OZZY_DEMO_EVENTS=1 in the environment plays changes from elsewhere instead, as another client or
 * the hardware would make them: no devices at first, after 2 s both arrive, after 4 s the clock
 * source goes to Internal, after 6 s the Xone:DB4 leaves, after 8 s it comes back.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

#include "ozzy_control.h"
#include "../../common/ozzy_devices.h"

/* How long a device setting takes, about what a handshake takes */
#define DEMO_RESTART_MS 800

static const char *const demo_models[] = { "Tascam US-600", "Xone:DB4" };
#define DEMO_DEVICES (sizeof(demo_models) / sizeof(demo_models[0]))

static struct ozzy_control_device demo[DEMO_DEVICES];
static bool demo_present[DEMO_DEVICES];
static bool demo_ready;
static void (*demo_changed)(void *ctx);
static void *demo_ctx;
static void demo_notify(void);

#ifdef _WIN32
static SRWLOCK demo_lock = SRWLOCK_INIT;
#define demo_lock_take() AcquireSRWLockExclusive(&demo_lock)
#define demo_lock_give() ReleaseSRWLockExclusive(&demo_lock)
#else
static pthread_mutex_t demo_lock = PTHREAD_MUTEX_INITIALIZER;
#define demo_lock_take() pthread_mutex_lock(&demo_lock)
#define demo_lock_give() pthread_mutex_unlock(&demo_lock)
#endif

static void demo_sleep_ms(unsigned int ms)
{
#ifdef _WIN32
	Sleep(ms);
#else
	struct timespec t = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000 };

	nanosleep(&t, NULL);
#endif
}

static const struct ozzy_device *demo_model(const char *name)
{
	unsigned int i;

	for (i = 0; i < OZZY_NUM_DEVICES; i++)
		if (ozzy_devices[i].name && !strcmp(ozzy_devices[i].name, name))
			return &ozzy_devices[i];
	return NULL;
}

/* The clock as a device with @dev's clock source reports it, and the record with it */
static void demo_clock(struct ozzy_control_device *dev)
{
	dev->clock.locked = dev->settings.value[OZZY_SETTING_CLOCK_SOURCE] == OZZY_CLOCK_AUTOMATIC;
	dev->clock.lock_rate = dev->clock.locked ? 44100 : 0;
	dev->sample_rate = dev->clock.locked ? 44100 : 48000;
	dev->info.sample_rate = dev->sample_rate;
	dev->info.clock_locked = dev->clock.locked;
	dev->info.lock_rate = dev->clock.lock_rate;
	dev->info.host_rates = dev->clock.locked ? OZZY_RATE_44K1 : dev->info.device_rates;
	if (dev->info.output == OZZY_INFO_OUTPUT_ISOC) {
		dev->info.safety_offset_frames = 3 * (dev->sample_rate / 1000 + 1);
		dev->host.safety_offset[0] = dev->info.safety_offset_frames;
	}
}

/* The transfer sizes and queues as the engine runs them (isoc: its own 2 in) */
static void demo_transfers(struct ozzy_control_device *d)
{
	struct ozzy_info *in = &d->info;
	bool isoc = in->output == OZZY_INFO_OUTPUT_ISOC;

	uint32_t out_queue = ozzy_pcm_queue(d->settings.value[OZZY_SETTING_PCM_OUT_QUEUE]);
	uint32_t in_queue = ozzy_pcm_queue(d->settings.value[OZZY_SETTING_PCM_IN_QUEUE]);

	in->transfer_frames[1] = ozzy_pcm_frames(true, d->settings.value[OZZY_SETTING_PCM_IN_FRAMES]);
	/* As the engines do: the isochronous family at its defaults (80 frames, 2) as its output */
	in->input_safety_offset_frames = isoc && in->transfer_frames[1] == 80 && in_queue == 2 ?
					 in->safety_offset_frames : in_queue * in->transfer_frames[1];
	in->pipes[OZZY_INFO_PIPE_PCM_IN].in_flight = (uint8_t)in_queue;
	if (!isoc) {
		in->transfer_frames[0] = ozzy_pcm_frames(false, d->settings.value[OZZY_SETTING_PCM_OUT_FRAMES]);
		in->safety_offset_frames = out_queue * in->transfer_frames[0];
		in->pipes[OZZY_INFO_PIPE_PCM_OUT].in_flight = (uint8_t)out_queue;
	}
	in->timestamp_frames = ozzy_anchor_frames(d->settings.value[OZZY_SETTING_IO_BUFFER_MAX],
						  isoc ? 1 : in->transfer_frames[0]);
	d->host.timestamp_period = in->timestamp_frames;
	d->host.safety_offset[0] = in->safety_offset_frames;
	d->host.safety_offset[1] = in->input_safety_offset_frames;
}

static void demo_pipe(struct ozzy_info_pipe *p, uint8_t address, uint8_t type, uint16_t max_packet, uint8_t interval,
		      uint8_t in_flight)
{
	p->address = address;
	p->transfer_type = type;
	p->max_packet = max_packet;
	p->interval = interval;
	p->in_flight = in_flight;
}

/* A record as the drivers publish them: the Xone family (DB4) or the isochronous one (US-600) */
static void demo_info(struct ozzy_control_device *d, const struct ozzy_device *m)
{
	struct ozzy_info *in = &d->info;
	bool isoc = m->family == OZZY_FAMILY_PLOYTEC_ISOC;

	d->has_info = true;
	in->version = OZZY_INFO_VERSION;
	in->size = sizeof(*in);
	in->starts = 1;
	in->vendor_id = m->vid;
	in->product_id = m->pid;
	in->family = m->family;
	in->status = m->status;
	in->usb_speed = OZZY_USB_SPEED_HIGH;
	in->interface_count = isoc ? 2 : 1;
	in->interfaces[0] = 0;
	in->interfaces[1] = 1;
	in->alt_setting = 1;
	in->output = isoc ? OZZY_INFO_OUTPUT_ISOC : OZZY_INFO_OUTPUT_PLOYTEC_INTERRUPT;
	in->feedback = isoc ? OZZY_INFO_FEEDBACK_COUNT : OZZY_INFO_FEEDBACK_NONE;
	in->firmware_valid = 1;
	in->firmware_chip = isoc ? 0x20 : 0x12;
	in->firmware[0] = 1;
	in->firmware[1] = isoc ? 2 : 4;
	in->firmware[2] = isoc ? 0 : 1;
	in->input_selector_valid = 1;
	in->input_selector = 0x02;
	in->sample_rate = 48000;
	in->boot_rate = 44100;
	in->device_rates = m->rates;
	in->host_rates = m->rates;
	in->in_channels = m->in_channels;
	in->out_channels = m->out_channels;
	in->transfer_frames[0] = isoc ? 49 : 80;
	in->transfer_frames[1] = 80;
	in->out_bytes_per_frame = isoc ? m->out_channels * 3 : 48;
	in->in_bytes_per_frame = 64;
	in->safety_offset_frames = isoc ? 147 : 320;
	in->timestamp_frames = 3200;
	snprintf(in->driver_version, sizeof(in->driver_version), "demo");
	/* Stats: the DB4 had one stop of the host's that held up both ways, and came back */
	d->has_stats = true;
	d->stats.version = OZZY_STATS_VERSION;
	d->stats.size = sizeof(d->stats);
	d->stats.dir[0].transfers = 3600000;
	d->stats.dir[1].transfers = 3600000;
	d->stats.dir[0].max_gap_us = isoc ? 1400 : 105300;
	d->stats.dir[1].max_gap_us = isoc ? 1500 : 105300;
	d->stats.dir[0].late = d->stats.dir[1].late = isoc ? 0 : 1;
	d->stats.recoveries = isoc ? 0 : 1;
	if (isoc) {
		demo_pipe(&in->pipes[OZZY_INFO_PIPE_PCM_OUT], 0x02, 1, 1024, 1, 3);
		demo_pipe(&in->pipes[OZZY_INFO_PIPE_PCM_IN], 0x86, 2, 512, 0, 2);
		demo_pipe(&in->pipes[OZZY_INFO_PIPE_FEEDBACK], 0x81, 1, 3, 4, 2);
		demo_pipe(&in->pipes[OZZY_INFO_PIPE_MIDI_IN], 0x83, 2, 512, 0, 4);
		demo_pipe(&in->pipes[OZZY_INFO_PIPE_MIDI_OUT], 0x04, 2, 512, 0, 1);
	} else {
		demo_pipe(&in->pipes[OZZY_INFO_PIPE_PCM_OUT], 0x05, 3, 964, 1, 4);
		demo_pipe(&in->pipes[OZZY_INFO_PIPE_PCM_IN], 0x86, 2, 512, 0, 4);
		demo_pipe(&in->pipes[OZZY_INFO_PIPE_MIDI_IN], 0x83, 3, 64, 1, 4);
	}
	snprintf(d->host.driver, sizeof(d->host.driver), "demo backend");
	d->host.buffer_frames = 512;
	d->host.latency[0] = 24;
	d->host.latency[1] = 24;
	d->host.safety_offset[0] = in->safety_offset_frames;
	d->host.safety_offset[1] = 80;
	d->host.timestamp_period = 3200;
	snprintf(d->host.format, sizeof(d->host.format), "32-bit float, 24-bit on the device");
}

/* Called with demo_lock held */
static void demo_init(void)
{
	unsigned int i, id;

	if (demo_ready)
		return;
	for (i = 0; i < DEMO_DEVICES; i++) {
		struct ozzy_control_device *d = &demo[i];
		const struct ozzy_device *m = demo_model(demo_models[i]);

		memset(d, 0, sizeof(*d));
		snprintf(d->name, sizeof(d->name), "%s", demo_models[i]);
		snprintf(d->id, sizeof(d->id), "demo.%u", i);
		d->handle = i;
		d->sample_rate = 48000;
		demo_present[i] = true;
		ozzy_settings_defaults(&d->settings);
		if (!m)
			continue;
		d->in_channels = m->in_channels;
		d->out_channels = m->out_channels;
		demo_info(d, m);
		/* The transfer settings come with the engine family, as its setting_quirks add them; the
		 * isochronous family's in queue starts at 2 (its setting_default) */
		for (id = 0; id < OZZY_NUM_SETTINGS; id++)
			if (ozzy_setting_values(m->quirks | (m->family == OZZY_FAMILY_PLOYTEC_ISOC ?
						OZZY_QUIRK_PCM_IN_FRAMES | OZZY_QUIRK_PCM_IN_QUEUE :
						OZZY_QUIRK_PCM_IN_FRAMES | OZZY_QUIRK_PCM_OUT_FRAMES |
						OZZY_QUIRK_PCM_IN_QUEUE | OZZY_QUIRK_PCM_OUT_QUEUE) |
						OZZY_QUIRK_CLOCK_ANCHORS,   /* as on macOS */
						m->in_channels, m->out_channels, id))
				d->available |= 1u << id;
		if (m->family == OZZY_FAMILY_PLOYTEC_ISOC)
			d->settings.value[OZZY_SETTING_PCM_IN_QUEUE] = 1;
		demo_transfers(d);
		d->has_clock = (d->available & (1u << OZZY_SETTING_CLOCK_SOURCE)) != 0;
		if (d->has_clock)
			demo_clock(d);
		demo_transfers(d);   /* the clock may have moved the output's offset */
	}
	demo_ready = true;
}

int ozzy_control_backend_list(struct ozzy_control_device *devs, int max)
{
	unsigned int i;
	int n = 0;

	demo_lock_take();
	demo_init();
	for (i = 0; i < DEMO_DEVICES && n < max; i++)
		if (demo_present[i])
			devs[n++] = demo[i];
	demo_lock_give();
	return n;
}

int ozzy_control_backend_set(const struct ozzy_control_device *dev, unsigned int id, unsigned int value)
{
	struct ozzy_control_device *d;

	if (dev->handle >= DEMO_DEVICES || id >= OZZY_NUM_SETTINGS) {
		fprintf(stderr, "%s: no such setting\n", dev->name);
		return -1;
	}
	/* A device setting restarts the device: it blocks for that long, as the drivers do */
	if (ozzy_setting_descs[id].kind == OZZY_SETTING_DEVICE)
		demo_sleep_ms(DEMO_RESTART_MS);
	demo_lock_take();
	demo_init();
	d = &demo[dev->handle];
	if (!demo_present[dev->handle] || !(d->available & (1u << id)) || value >= ozzy_control_values(d, id) ||
	    !ozzy_control_value_fits(d, id, value)) {
		demo_lock_give();
		fprintf(stderr, "%s: %u is not a value of %s\n", dev->name, value, ozzy_setting_descs[id].name);
		return -1;
	}
	d->settings.value[id] = (uint8_t)value;
	if (id == OZZY_SETTING_CLOCK_SOURCE)
		demo_clock(d);
	demo_transfers(d);
	if (ozzy_setting_descs[id].kind == OZZY_SETTING_DEVICE)
		d->info.starts++;   /* a device setting is a handshake */
	demo_lock_give();
	demo_notify();
	return 0;
}

/* The front end's callback, one call at a time as ozzy_control_backend_watch promises (a set and
 * the events thread may both call it) */
static void demo_notify(void)
{
#ifdef _WIN32
	static SRWLOCK lock = SRWLOCK_INIT;

	AcquireSRWLockExclusive(&lock);
	if (demo_changed)
		demo_changed(demo_ctx);
	ReleaseSRWLockExclusive(&lock);
#else
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

	pthread_mutex_lock(&lock);
	if (demo_changed)
		demo_changed(demo_ctx);
	pthread_mutex_unlock(&lock);
#endif
}

/* OZZY_DEMO_EVENTS: the changes from elsewhere */
static void demo_events(void)
{
	demo_sleep_ms(2000);
	demo_lock_take();
	demo_present[0] = demo_present[1] = true;
	demo_lock_give();
	demo_notify();

	demo_sleep_ms(2000);
	demo_lock_take();
	demo[0].settings.value[OZZY_SETTING_CLOCK_SOURCE] = OZZY_CLOCK_INTERNAL;
	demo_clock(&demo[0]);
	demo_lock_give();
	demo_notify();

	demo_sleep_ms(2000);
	demo_lock_take();
	demo_present[1] = false;
	demo_lock_give();
	demo_notify();

	demo_sleep_ms(2000);
	demo_lock_take();
	demo_present[1] = true;
	demo_lock_give();
	demo_notify();
}

#ifdef _WIN32
static DWORD WINAPI demo_events_thread(LPVOID arg)
{
	(void)arg;
	demo_events();
	return 0;
}
#else
static void *demo_events_thread(void *arg)
{
	(void)arg;
	demo_events();
	return NULL;
}
#endif

int ozzy_control_backend_watch(void (*changed)(void *ctx), void *ctx)
{
	const char *events = getenv("OZZY_DEMO_EVENTS");

	demo_lock_take();
	demo_init();
	demo_changed = changed;
	demo_ctx = ctx;
	demo_lock_give();
	if (events && *events == '1') {
		demo_lock_take();
		demo_present[0] = demo_present[1] = false;
		demo_lock_give();
#ifdef _WIN32
		HANDLE t = CreateThread(NULL, 0, demo_events_thread, NULL, 0, NULL);

		if (t)
			CloseHandle(t);
#else
		pthread_t t;

		if (!pthread_create(&t, NULL, demo_events_thread, NULL))
			pthread_detach(t);
#endif
	}
	return 0;
}
