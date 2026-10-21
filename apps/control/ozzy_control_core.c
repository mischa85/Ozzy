// SPDX-License-Identifier: MIT
/*
 * Ozzy control: the platform-neutral core every front end shares (ozzy_control.h): setting and value
 * names, finding devices, settings and values by what a user typed or picked, and the facts
 * about a device (ozzy_control_facts) that ozzy-cli and the ozzy app both show.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ozzy_control.h"
#include "../../common/ozzy_devices.h"

void ozzy_control_slug(const char *name, char *out, size_t size)
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

	ozzy_control_slug(arg, a, sizeof(a));
	ozzy_control_slug(name, b, sizeof(b));
	return strcmp(a, b) == 0;
}

/* @arg as a non-negative number, or -1 */
static long number(const char *arg)
{
	char *end;
	long v = strtol(arg, &end, 10);

	return (*arg && !*end && v >= 0) ? v : -1;
}

unsigned int ozzy_control_values(const struct ozzy_control_device *dev, unsigned int id)
{
	return (dev->available & (1u << id)) ? ozzy_setting_count(id, dev->in_channels, dev->out_channels) : 0;
}

bool ozzy_control_value_fits(const struct ozzy_control_device *dev, unsigned int id, unsigned int value)
{
	const struct ozzy_info_pipe *out = &dev->info.pipes[OZZY_INFO_PIPE_PCM_OUT];
	const struct ozzy_info_pipe *in = &dev->info.pipes[OZZY_INFO_PIPE_PCM_IN];

	return !dev->has_info ||
	       ozzy_setting_value_fits(id, value,
				       ozzy_pcm_step(out->transfer_type, out->max_packet, dev->info.out_bytes_per_frame),
				       ozzy_pcm_step(in->transfer_type, in->max_packet, dev->info.in_bytes_per_frame));
}

const struct ozzy_control_device *ozzy_control_pick(const struct ozzy_control_device *devs, int n, const char *arg)
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
	ozzy_control_slug(arg, a, sizeof(a));
	for (i = 0; i < n; i++) {
		ozzy_control_slug(devs[i].name, b, sizeof(b));
		if (strstr(b, a))
			return &devs[i];
	}
	return NULL;
}

int ozzy_control_find_setting(const struct ozzy_control_device *dev, const char *arg)
{
	long id = number(arg);
	unsigned int k;

	for (k = 0; id < 0 && k < OZZY_NUM_SETTINGS; k++)
		if (names(arg, ozzy_setting_descs[k].name))
			id = k;
	return (id >= 0 && id < OZZY_NUM_SETTINGS && ozzy_control_values(dev, (unsigned int)id)) ? (int)id : -1;
}

int ozzy_control_find_value(const struct ozzy_control_device *dev, unsigned int id, const char *arg)
{
	unsigned int n = ozzy_control_values(dev, id), k;
	char a[96], b[96];
	size_t len;
	long v;

	/* A name, or its first word ("10" for "10 frames") */
	ozzy_control_slug(arg, a, sizeof(a));
	len = strlen(a);
	for (k = 0; len && k < n; k++) {
		if (!ozzy_control_value_fits(dev, id, k))
			continue;
		ozzy_control_slug(ozzy_setting_descs[id].value_names[k], b, sizeof(b));
		if (!strcmp(a, b) || (!strncmp(a, b, len) && b[len] == '-'))
			return (int)k;
	}
	/* Else a value's number, unless the values are named by numbers (frames, transfers): there
	 * a number means one of those, and none fits */
	if (n && isdigit((unsigned char)ozzy_setting_descs[id].value_names[0][0]))
		return -1;
	v = number(arg);
	return (v >= 0 && (unsigned long)v < n && ozzy_control_value_fits(dev, id, (unsigned int)v)) ? (int)v : -1;
}

bool ozzy_control_reread(const char *id, struct ozzy_control_device *out)
{
	struct ozzy_control_device devs[OZZY_CONTROL_MAX_DEVICES];
	int i, n = ozzy_control_backend_list(devs, OZZY_CONTROL_MAX_DEVICES);

	for (i = 0; i < n; i++) {
		if (strcmp(devs[i].id, id) == 0) {
			*out = devs[i];
			return true;
		}
	}
	return false;
}

bool ozzy_control_same(const struct ozzy_control_device *a, const struct ozzy_control_device *b)
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
	/* Both come zeroed and filled by the backend, padding included */
	/* The stats by their incidents: the transfer counts move all the time */
	if (a->has_stats != b->has_stats ||
	    (a->has_stats && ozzy_stats_incidents(&a->stats) != ozzy_stats_incidents(&b->stats)))
		return false;
	return a->has_info == b->has_info && (!a->has_info || !memcmp(&a->info, &b->info, sizeof(a->info))) &&
	       !memcmp(&a->host, &b->host, sizeof(a->host));
}

bool ozzy_control_take_info(struct ozzy_info *out, const void *raw, size_t size)
{
	const struct ozzy_info *in = raw;

	if (!raw || size < sizeof(*in) || in->version != OZZY_INFO_VERSION || in->size != sizeof(*in))
		return false;
	memcpy(out, in, sizeof(*out));
	out->driver_version[sizeof(out->driver_version) - 1] = 0;
	return true;
}

bool ozzy_control_take_stats(struct ozzy_stats *out, const void *raw, size_t size)
{
	const struct ozzy_stats *in = raw;

	if (!raw || size < sizeof(*in) || in->version != OZZY_STATS_VERSION || in->size != sizeof(*in))
		return false;
	memcpy(out, in, sizeof(*out));
	return true;
}

/* --- The facts --- */

const char *const ozzy_control_group_names[OZZY_CONTROL_GROUPS] = {
	"Device", "Audio", "Stream", "Stream health", "Endpoints", "Operating system"
};

struct facts {
	struct ozzy_control_fact *out;
	int max;
	int n;
	enum ozzy_control_group group;
	uint32_t rate;           /* Hz, for durations; 0: none shown */
};

static void fact(struct facts *f, const char *label, const char *fmt, ...)
{
	struct ozzy_control_fact *x;
	va_list ap;

	if (f->n >= f->max)
		return;
	x = &f->out[f->n++];
	x->group = f->group;
	snprintf(x->label, sizeof(x->label), "%s", label);
	va_start(ap, fmt);
	vsnprintf(x->value, sizeof(x->value), fmt, ap);
	va_end(ap);
}

/* @hz as "44.1 kHz" / "48 kHz" */
static const char *khz(uint32_t hz, char *buf, size_t size)
{
	if (hz % 1000)
		snprintf(buf, size, "%u.%u kHz", (unsigned int)(hz / 1000), (unsigned int)(hz % 1000) / 100);
	else
		snprintf(buf, size, "%u kHz", (unsigned int)(hz / 1000));
	return buf;
}

/* @frames as "160 frames (3.33 ms)" at the rate shown, or "160 frames" */
static const char *frames(const struct facts *f, uint32_t n, char *buf, size_t size)
{
	if (f->rate)
		snprintf(buf, size, "%u frame%s (%.2f ms)", n, n == 1 ? "" : "s", n * 1000.0 / f->rate);
	else
		snprintf(buf, size, "%u frame%s", n, n == 1 ? "" : "s");
	return buf;
}

/* OZZY_RATE_* bits as "44.1, 48, 88.2, 96 kHz" */
static const char *rate_list(uint32_t bits, char *buf, size_t size)
{
	struct ozzy_device every;
	uint32_t hz[OZZY_MAX_RATES];
	unsigned int n, i;
	size_t len = 0;

	memset(&every, 0, sizeof(every));
	every.rates = (1u << OZZY_MAX_RATES) - 1;
	n = ozzy_device_rates(&every, hz);
	buf[0] = 0;
	for (i = 0; i < n && len + 16 < size; i++) {
		if (!(bits & (1u << i)))
			continue;
		if (hz[i] % 1000)
			len += (size_t)snprintf(buf + len, size - len, "%s%u.%u", len ? ", " : "",
						(unsigned int)(hz[i] / 1000), (unsigned int)(hz[i] % 1000) / 100);
		else
			len += (size_t)snprintf(buf + len, size - len, "%s%u", len ? ", " : "", (unsigned int)(hz[i] / 1000));
	}
	if (len + 5 < size)
		snprintf(buf + len, size - len, " kHz");
	return buf;
}

/* How often a pipe is serviced: bInterval by speed and type (USB 2.0 9.6.6) */
static void interval_text(const struct ozzy_info_pipe *p, uint8_t speed, char *buf, size_t size)
{
	uint32_t us;

	buf[0] = 0;
	if (p->transfer_type == 2 || !p->interval)   /* bulk: as the bus has room */
		return;
	if (speed == OZZY_USB_SPEED_HIGH || speed == OZZY_USB_SPEED_SUPER ||
	    (p->transfer_type == 1 && speed != OZZY_USB_SPEED_LOW))
		us = (speed == OZZY_USB_SPEED_HIGH || speed == OZZY_USB_SPEED_SUPER ? 125u : 1000u)
		     << (p->interval > 16 ? 15 : p->interval - 1);
	else
		us = p->interval * 1000u;   /* Full / Low Speed interrupt: bInterval ms */
	if (us < 1000)
		snprintf(buf, size, ", %u us", (unsigned int)us);
	else
		snprintf(buf, size, ", %u ms", (unsigned int)(us / 1000));
}

int ozzy_control_facts(const struct ozzy_control_device *dev, struct ozzy_control_fact *out, int max)
{
	static const char *const families[] = { "Ploytec, bulk / interrupt PCM out", "Ploytec, isochronous PCM out",
						 "USB MIDI class" };
	static const char *const speeds[] = { "unknown", "USB Low Speed (1.5 Mbit/s)", "USB Full Speed (12 Mbit/s)",
					      "USB High Speed (480 Mbit/s)", "USB SuperSpeed (5 Gbit/s or more)" };
	static const char *const outputs[] = { "none", "Ploytec 512-byte bulk layout",
					       "Ploytec 482-byte interrupt layout", "isochronous" };
	static const char *const feedbacks[] = { "", ", device feedback (frames per packet)",
						 ", device feedback (16.16 per microframe)" };
	static const char *const types[] = { "control", "isoc", "bulk", "interrupt" };
	static const char *const roles[OZZY_INFO_PIPES] = { "PCM out", "PCM in", "MIDI in", "MIDI out", "Feedback" };
	const struct ozzy_info *in = &dev->info;
	const struct ozzy_control_host *h = &dev->host;
	struct facts f = { out, max, 0, OZZY_CONTROL_GROUP_DEVICE, 0 };
	char a[64], b[64], c[64];
	unsigned int i;
	size_t len;

	f.rate = dev->has_info && in->sample_rate ? in->sample_rate : dev->sample_rate;

	/* Device */
	if (dev->has_info) {
		fact(&f, "USB ID", "%04X:%04X", in->vendor_id, in->product_id);
		fact(&f, "Support", "%s", in->status == OZZY_STATUS_SUPPORTED ? "supported (tested)" :
			 "experimental (needs testers)");
		if (in->family < sizeof(families) / sizeof(families[0]))
			fact(&f, "Protocol", "%s", families[in->family]);
		if (in->firmware_valid)
			fact(&f, "Firmware", "%u.%u.%u (chip 0x%02X)", in->firmware[0], in->firmware[1], in->firmware[2],
			     in->firmware_chip);
		if (in->usb_speed && in->usb_speed < sizeof(speeds) / sizeof(speeds[0]))
			fact(&f, "Connection", "%s", speeds[in->usb_speed]);
		len = 0;
		a[0] = 0;
		for (i = 0; i < in->interface_count && i < 4; i++)
			len += (size_t)snprintf(a + len, sizeof(a) - len, "%s%u", i ? ", " : "", in->interfaces[i]);
		if (in->interface_count)
			fact(&f, in->interface_count == 1 ? "Interface" : "Interfaces", "%s, alternate setting %u", a,
			     in->alt_setting);
		fact(&f, "Handshakes", "%u since connected", in->starts);
	}
	if (h->driver[0] || (dev->has_info && in->driver_version[0]))
		fact(&f, "Driver", "%s%s%s", h->driver, h->driver[0] && dev->has_info && in->driver_version[0] ? ", build " : "",
		     dev->has_info ? in->driver_version : "");
	if (!dev->id_internal)
		fact(&f, "Driver ID", "%s", dev->id);

	/* Audio */
	f.group = OZZY_CONTROL_GROUP_AUDIO;
	if (f.rate)
		fact(&f, "Sample rate", "%s", khz(f.rate, a, sizeof(a)));
	if (dev->has_info && in->device_rates) {
		fact(&f, "Rates", "%s", rate_list(in->device_rates, a, sizeof(a)));
		if (in->host_rates && in->host_rates != in->device_rates)
			fact(&f, "Rates now", "%s%s", rate_list(in->host_rates, b, sizeof(b)),
			     in->clock_locked ? " (locked to S/PDIF)" : "");
	}
	if (dev->has_info && in->boot_rate)
		fact(&f, "Rate when attached", "%s", khz(in->boot_rate, a, sizeof(a)));
	if (dev->in_channels || dev->out_channels)
		fact(&f, "Channels", "%u in / %u out", dev->in_channels, dev->out_channels);
	if (dev->has_clock && dev->clock.locked && dev->clock.lock_rate)
		fact(&f, "Digital input", "locked to S/PDIF at %s", khz(dev->clock.lock_rate, a, sizeof(a)));
	else if (dev->has_clock && dev->clock.locked)
		fact(&f, "Digital input", "locked to S/PDIF");
	else if (dev->has_clock)
		fact(&f, "Digital input", "not locked (internal clock)");
	if (dev->has_info && in->input_selector_valid)
		fact(&f, "Input selector", "0x%02X", in->input_selector);

	/* Stream */
	f.group = OZZY_CONTROL_GROUP_STREAM;
	if (dev->has_info && (in->transfer_frames[0] || in->transfer_frames[1])) {
		/*
		 * What the transfers come to on USB. Out: a frame waits behind every transfer queued (the
		 * safety offset), and the host may be late by all but one of them. In: a frame is there
		 * once its one transfer completes; the others queued are buffers the device can fill
		 * while the host is late. Both have the device's own buffer besides, which isn't known.
		 */
		uint32_t out_q = in->pipes[OZZY_INFO_PIPE_PCM_OUT].in_flight, in_q = in->pipes[OZZY_INFO_PIPE_PCM_IN].in_flight;
		/* Isochronous output is paced by the feedback: its queue is the safety offset */
		uint32_t out_queued = in->output == OZZY_INFO_OUTPUT_ISOC ? in->safety_offset_frames :
				      out_q * in->transfer_frames[0];
		uint32_t out_margin = out_queued > in->transfer_frames[0] ? out_queued - in->transfer_frames[0] : 0;
		uint32_t in_margin = in_q > 1 ? (in_q - 1) * in->transfer_frames[1] : 0;

		if (in->transfer_frames[0] && out_q)
			fact(&f, "Out latency (USB)", "%s queued ahead", frames(&f, out_queued, a, sizeof(a)));
		if (in->transfer_frames[1])
			fact(&f, "In latency (USB)", "%s, one transfer", frames(&f, in->transfer_frames[1], a, sizeof(a)));
		if ((in->transfer_frames[0] && out_q) || in->transfer_frames[1])
			fact(&f, "Margin", "out %.2f ms, in %.2f ms, plus the device's own buffer",
			     f.rate ? out_margin * 1000.0 / f.rate : 0.0, f.rate ? in_margin * 1000.0 / f.rate : 0.0);
		if (in->output < sizeof(outputs) / sizeof(outputs[0]) && in->output != OZZY_INFO_OUTPUT_NONE)
			fact(&f, "Output", "%s%s", outputs[in->output],
			     in->feedback < sizeof(feedbacks) / sizeof(feedbacks[0]) ? feedbacks[in->feedback] : "");
		if (in->transfer_frames[0])
			fact(&f, "Out transfers", "%s, %u queued, %u bytes/frame",
			     frames(&f, in->transfer_frames[0], a, sizeof(a)), in->pipes[OZZY_INFO_PIPE_PCM_OUT].in_flight,
			     in->out_bytes_per_frame);
		if (in->transfer_frames[1])
			fact(&f, "In transfers", "%s, %u queued, %u bytes/frame",
			     frames(&f, in->transfer_frames[1], a, sizeof(a)), in->pipes[OZZY_INFO_PIPE_PCM_IN].in_flight,
			     in->in_bytes_per_frame);
		if (in->timestamp_frames)
			fact(&f, "Clock anchors", "every %s", frames(&f, in->timestamp_frames, a, sizeof(a)));
	}

	/*
	 * Stream health: since the driver took the device. Late is a completion after a gap the
	 * queued transfers didn't cover: the host stopped serving the device for a while (the USB
	 * side's xrun; not the client's).
	 */
	f.group = OZZY_CONTROL_GROUP_HEALTH;
	if (dev->has_stats) {
		static const char *const dirs[2] = { "Out", "In" };
		unsigned int channels[2] = { dev->out_channels, dev->in_channels };

		for (i = 0; i < 2; i++) {
			const struct ozzy_stats_dir *s = &dev->stats.dir[i];

			if (!channels[i] && !s->transfers)
				continue;
			fact(&f, dirs[i], "%u late, %u errors, %u short, longest gap %.1f ms", s->late, s->errors,
			     s->short_transfers, s->max_gap_us / 1000.0);
		}
		if (dev->stats.recovery_failures)
			fact(&f, "Recoveries", "%u (%u failed)", dev->stats.recoveries, dev->stats.recovery_failures);
		else
			fact(&f, "Recoveries", "%u", dev->stats.recoveries);
	}

	/* Endpoints */
	f.group = OZZY_CONTROL_GROUP_ENDPOINTS;
	for (i = 0; dev->has_info && i < OZZY_INFO_PIPES; i++) {
		const struct ozzy_info_pipe *p = &in->pipes[i];

		if (!p->address)
			continue;
		interval_text(p, in->usb_speed, b, sizeof(b));
		if (p->in_flight)
			snprintf(c, sizeof(c), ", %u queued", p->in_flight);
		else
			c[0] = 0;
		fact(&f, roles[i], "0x%02X %s %s, %u B%s%s", p->address, (p->address & 0x80) ? "IN" : "OUT",
		     types[p->transfer_type & 3], p->max_packet, b, c);
	}

	/* Operating system */
	f.group = OZZY_CONTROL_GROUP_HOST;
	if (h->driver[0])
		fact(&f, "In use", "%s", h->running ? "yes, a client streams" : "no");
	if (h->client[0][0])
		fact(&f, "Playing", "%s", h->client[0]);
	if (h->client[1][0])
		fact(&f, "Recording", "%s", h->client[1]);
	if (h->clients[0])
		fact(&f, "Clients", "%s", h->clients);
	if (h->format[0])
		fact(&f, "Format", "%s", h->format);
	if (h->buffer_frames)
		fact(&f, h->ring_frames ? "Period" : "IO buffer", "%s", frames(&f, h->buffer_frames, a, sizeof(a)));
	if (h->ring_frames)
		fact(&f, "Buffer", "%s", frames(&f, h->ring_frames, a, sizeof(a)));
	if (h->latency[0] || h->latency[1])
		fact(&f, "Latency reported", "out %u, in %u frames", h->latency[0], h->latency[1]);
	if (h->safety_offset[0] || h->safety_offset[1])
		fact(&f, "Safety offset reported", "out %u, in %u frames", h->safety_offset[0], h->safety_offset[1]);
	if (h->timestamp_period)
		fact(&f, "Timestamp period", "%s", frames(&f, h->timestamp_period, a, sizeof(a)));
	if (h->buffer_frames && !h->ring_frames && f.rate) {
		uint32_t o = h->buffer_frames + h->safety_offset[0] + h->latency[0];
		uint32_t i2 = h->buffer_frames + h->safety_offset[1] + h->latency[1];

		fact(&f, "Round trip (about)", "%.1f ms (out %.1f, in %.1f)",
		     (o + i2) * 1000.0 / f.rate, o * 1000.0 / f.rate, i2 * 1000.0 / f.rate);
	}
	return f.n;
}
