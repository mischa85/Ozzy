/* SPDX-License-Identifier: MIT */
/*
 * Ozzy Device Settings: the settings model shared by every platform
 *
 * The user-facing settings a device can have, their values and names. Which device has
 * which setting follows its quirk bits (ozzy_quirks.h) and, for routing, its channel
 * counts. Two kinds:
 *   - device settings (clock source, S/PDIF out mode, codec switches): the family's
 *     protocol code sends them in its handshake (ploytec_handshake.c), so a change restarts
 *     streaming (as the PHASE X64's own panels stop streaming around a codec change)
 *   - routes and output pairs: which stereo pair goes where, applied by the host's codecs
 *     (ozzy_route.h, the family's encoder), no request and no restart
 * Platforms only map the settings onto their own tooling:
 *   - Linux: one ALSA kcontrol per setting, named after desc->name
 *   - macOS: OzzyHAL; the clock source as kAudioDevicePropertyClockSource, every setting as
 *     the custom property kOzzyPropertySetting; device settings go to the kext with
 *     kOzzyMethodApplySetting
 *
 * Pure C with no platform dependencies -- usable in both kernel space
 * and userspace on any platform.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_SETTINGS_H
#define OZZY_SETTINGS_H

#if defined(__KERNEL__) && defined(__linux__)
#include <linux/types.h>
#else
#include <stdint.h>
#include <stdbool.h>
#endif

#include "ozzy_quirks.h"
#include "ozzy_route.h"

#ifdef __cplusplus
extern "C" {
#endif

enum ozzy_setting {
	OZZY_SETTING_CLOCK_SOURCE,
	OZZY_SETTING_SPDIF_OUT_MODE,
	OZZY_SETTING_OUT_ROUTE_1,     /* source of device output pair 1 (outputs 1-2) */
	OZZY_SETTING_OUT_ROUTE_2,
	OZZY_SETTING_IN_ROUTE_1,      /* source of host capture pair 1 (capture 1-2) */
	OZZY_SETTING_IN_ROUTE_2,
	OZZY_SETTING_IN_ROUTE_3,
	OZZY_SETTING_SPDIF_MIRROR,    /* the playback pair the S/PDIF output mirrors (ESU1808) */
	OZZY_SETTING_PCM_OUT_FRAMES,  /* frames per PCM out transfer (Ploytec bulk / interrupt) */
	OZZY_SETTING_PCM_IN_FRAMES,   /* frames per PCM in transfer (Ploytec) */
	OZZY_SETTING_PCM_OUT_QUEUE,   /* PCM out transfers in flight (Ploytec bulk / interrupt) */
	OZZY_SETTING_PCM_IN_QUEUE,    /* PCM in transfers in flight (Ploytec bulk / interrupt) */
	OZZY_SETTING_IO_BUFFER_MAX,   /* the largest IO buffer the host may use: the clock anchors' spacing */
	OZZY_SETTING_WM_INPUT_12,     /* WM8776 codecs (PHASE X64): what inputs 1-2 record */
	OZZY_SETTING_WM_INPUT_34,
	OZZY_SETTING_WM_PLAYBACK_12,  /* outputs 1-2 play the host */
	OZZY_SETTING_WM_MONO_12,      /* ... in mono */
	OZZY_SETTING_WM_DIRECT_12,    /* outputs 1-2 play inputs 1-2 (direct monitoring) */
	OZZY_SETTING_WM_PLAYBACK_34,
	OZZY_SETTING_WM_MONO_34,
	OZZY_SETTING_WM_DIRECT_34,
	OZZY_SETTING_WM_MODULE_34,    /* outputs 3-4 play the sound module */
	OZZY_NUM_SETTINGS
};

enum ozzy_setting_kind {
	OZZY_SETTING_DEVICE,          /* taken when the engine starts: the handshake, the transfers */
	OZZY_SETTING_OUT_ROUTE,       /* value: host playback pair */
	OZZY_SETTING_IN_ROUTE,        /* value: device input pair */
	OZZY_SETTING_OUT_PAIR,        /* value: host playback pair, used by the family's encoder */
};

/*
 * OZZY_SETTING_CLOCK_SOURCE values, the official driver's ClockSource (AjExtData[1]). Tascam's
 * US-1641/1800/2000 Control Panel offers "Automatic" (index 0, the default: the digital input
 * when it has a valid signal, else internal) and "Internal" (index 1: always internal), and
 * passes the index to setClockSource.
 */
enum ozzy_clock_source {
	OZZY_CLOCK_AUTOMATIC = 0,
	OZZY_CLOCK_INTERNAL  = 1,
	OZZY_NUM_CLOCK_SOURCES
};

/*
 * OZZY_SETTING_SPDIF_OUT_MODE values: the S/PDIF output's channel status (the official
 * driver's DigitalOutMode 0/1). Mode 1 sets bit 0 of channel status byte 0, IEC 60958's
 * professional flag, so it is named Professional (from the standard, not the driver).
 */
enum ozzy_spdif_out_mode {
	OZZY_SPDIF_OUT_CONSUMER     = 0,
	OZZY_SPDIF_OUT_PROFESSIONAL = 1,
	OZZY_NUM_SPDIF_OUT_MODES
};

/*
 * The WM8776 settings (OZZY_QUIRK_WM8776, Musonik PHASE X64): the official control panels'
 * switches, which the handshake writes to the two codecs (ploytec_handshake.c). Defaults: what
 * the Windows driver writes without saved ones. Inputs: one source per pair, as the Windows
 * panel offers them (the Mac panel can mix several, except Phono). Phono is recorded flat: the
 * RIAA curve is the user's to add. The unit's side switches (Aux/Phono, Mic/Line, Line/Hi-Z)
 * pick the jack before this and have to match.
 */
enum ozzy_wm_input_12 {
	OZZY_WM_IN12_MIC_LINE,
	OZZY_WM_IN12_PHONO,
	OZZY_WM_IN12_AUX,
	OZZY_WM_IN12_MODULE,
	OZZY_WM_IN12_OUT34,           /* what outputs 3-4 play (their analog signal) */
	OZZY_WM_IN12_OFF,
	OZZY_NUM_WM_IN12
};

enum ozzy_wm_input_34 {
	OZZY_WM_IN34_PHONO,
	OZZY_WM_IN34_AUX,
	OZZY_WM_IN34_MODULE,
	OZZY_WM_IN34_OUT12,
	OZZY_WM_IN34_OFF,
	OZZY_NUM_WM_IN34
};

/* OZZY_SETTING_WM_DIRECT_12: the Mac panel offers Mono only with Mic/Line and Direct on */
enum ozzy_wm_direct {
	OZZY_WM_DIRECT_OFF,
	OZZY_WM_DIRECT_ON,
	OZZY_WM_DIRECT_MONO,
	OZZY_NUM_WM_DIRECT
};

#define OZZY_NUM_OFF_ON 2

/*
 * OZZY_SETTING_PCM_OUT_FRAMES / _IN_FRAMES values: how many of the device's sub-packets one PCM
 * transfer carries, so how many frames (value + 1 sub-packets: 10 frames each out, 8 in), so
 * how far ahead of the device the driver works (transfers queued x frames: the latency). Only
 * whole packets of the endpoint work (ozzy_pcm_step: on the DB4 20 frames out, 16 in).
 * .claude/re/ploytec_bulk_devices.md. Ozzy offers up to 200 frames, the official driver's size.
 * The default, 80 frames each way, is what ran before the setting.
 */
#define OZZY_NUM_PCM_OUT_FRAMES 20    /* 10 ... 200 */
#define OZZY_NUM_PCM_IN_FRAMES  25    /* 8 ... 200 */

/*
 * OZZY_SETTING_PCM_OUT_QUEUE / _IN_QUEUE values: how many PCM transfers the driver keeps in
 * flight, value + 1 (2-4; value 0, one, isn't offered and runs as two). With the transfer size it
 * is the latency on USB: queue x frames. The device paces the stream itself (it refuses OUT
 * packets until it has room), so its own buffer covers the gap while the next transfer is being
 * submitted; one in flight with 20-frame out transfers works on the DB4 under Linux, but not
 * under macOS, whose USB stack puts the next transfer on the wire too late: the DB4 runs dry once
 * per transfer, and on Windows it glitches too (.claude/re/ploytec_bulk_devices.md). The
 * official driver never runs one either. The values keep their numbers, so a stored setting
 * means what it meant. Out, each
 * transfer queued is latency (a frame waits behind them all); in, the frames are there once
 * their one transfer completes, and the others queued are margin, buffers the device can fill
 * while the host is late. The default, 4, is what ran before the setting (the isochronous
 * family's in queue: 2, its engine's setting_default).
 */
#define OZZY_NUM_PCM_QUEUES 4

#define OZZY_PCM_QUEUE_MIN 1           /* the lowest value offered: two in flight */

static inline uint32_t ozzy_pcm_queue(unsigned int value)
{
	if (value < OZZY_PCM_QUEUE_MIN)
		value = OZZY_PCM_QUEUE_MIN;
	return value < OZZY_NUM_PCM_QUEUES ? value + 1 : OZZY_NUM_PCM_QUEUES;
}

/*
 * OZZY_SETTING_IO_BUFFER_MAX values: the largest IO buffer a host that reads the engine's clock
 * anchors (macOS: CoreAudio's zero timestamps) may use, which sets how far apart the anchors
 * are. CoreAudio caps the IO buffer at 3/8 of the anchors' spacing (measured: 640 -> 240
 * frames), so the spacing is 8/3 of the buffer, rounded up to whole out transfers: anchors close
 * together cap the apps' buffer, anchors far apart leave the host's clock running on its own
 * longer between them. The default, 1200 (anchors every 3200 frames), is what ran before the
 * setting. Only offered where the platform reads the anchors (ozzy_engine_set_anchors).
 */
#define OZZY_NUM_IO_BUFFERS 4

static inline uint32_t ozzy_io_buffer_frames(unsigned int value)
{
	return 300u << (value < OZZY_NUM_IO_BUFFERS ? value : 2);
}

/* The anchors' spacing for value @value, in whole transfers of @unit frames (0: 1) */
static inline uint32_t ozzy_anchor_frames(unsigned int value, uint32_t unit)
{
	uint32_t spacing = ozzy_io_buffer_frames(value) * 8 / 3;

	unit = unit ? unit : 1;
	return (spacing + unit - 1) / unit * unit;
}

/* The frames of value @value of the out (@in false) or in setting; out of range: the default */
static inline uint32_t ozzy_pcm_frames(bool in, unsigned int value)
{
	if (value >= (in ? OZZY_NUM_PCM_IN_FRAMES : OZZY_NUM_PCM_OUT_FRAMES) + 0u)
		value = in ? 9 : 7;
	return (value + 1) * (in ? 8 : 10);
}

/*
 * ozzy_pcm_step - What a PCM transfer's frames must be a multiple of, on an endpoint of
 * @transfer_type (bmAttributes & 3) with @max_packet bytes per service interval and frames of
 * @frame_bytes: the frames in one whole packet. On bulk and interrupt the device takes packets
 * whole, both ways (DB4 fw 1.4.1, .claude/re/ploytec_bulk_devices.md): in, it fills 1024-byte
 * packets of 16 frames, and a request for 8 is overrun (babble); out, it reads every packet as a
 * 964-byte one of 20 frames, and a 10-frame transfer (one 482-byte half) plays garbage.
 * 0: no step (isochronous, or not known).
 */
static inline uint32_t ozzy_pcm_step(uint8_t transfer_type, uint32_t max_packet, uint32_t frame_bytes)
{
	if ((transfer_type != 2 && transfer_type != 3) || !frame_bytes)
		return 0;
	return max_packet / frame_bytes;
}

/* Whether a device whose PCM steps are @out_step / @in_step (ozzy_pcm_step) can take value
 * @value of setting @id: the transfer sizes depend on the device; one transfer in flight is
 * offered to none (ozzy_pcm_queue) */
static inline bool ozzy_setting_value_fits(unsigned int id, unsigned int value, uint32_t out_step, uint32_t in_step)
{
	if (id == OZZY_SETTING_PCM_OUT_QUEUE || id == OZZY_SETTING_PCM_IN_QUEUE)
		return value >= OZZY_PCM_QUEUE_MIN;
	if (id == OZZY_SETTING_PCM_OUT_FRAMES)
		return !out_step || ozzy_pcm_frames(false, value) % out_step == 0;
	if (id == OZZY_SETTING_PCM_IN_FRAMES)
		return !in_step || ozzy_pcm_frames(true, value) % in_step == 0;
	return true;
}

/*
 * struct ozzy_setting_desc - One setting: its name, values and which devices have it.
 * @name:        the setting's name, also the ALSA control name ("Clock Source", as
 *               snd-usb-audio names its clock selector)
 * @value_names: names indexed by value (routes, output pairs: OZZY_ROUTE_MAX_PAIRS of them)
 * @num_values:  device settings: how many values; others: see ozzy_setting_count
 * @def:         the value a device starts with (device settings: what the handshake sent
 *               before settings existed; routes: the pair itself)
 * @quirk:       the device has the setting if its quirks have this bit
 * @kind:        enum ozzy_setting_kind
 * @pair:        routes: the pair this setting routes (0 = channels 1-2)
 */
struct ozzy_setting_desc {
	const char *name;
	const char *const *value_names;
	uint8_t num_values;
	uint8_t def;
	uint32_t quirk;
	uint8_t kind;
	uint8_t pair;
};

static const char *const ozzy_clock_source_names[OZZY_NUM_CLOCK_SOURCES] = { "Automatic", "Internal" };
static const char *const ozzy_spdif_out_mode_names[OZZY_NUM_SPDIF_OUT_MODES] = { "Consumer", "Professional" };
static const char *const ozzy_playback_pair_names[OZZY_ROUTE_MAX_PAIRS] = {
	"Playback 1-2", "Playback 3-4", "Playback 5-6", "Playback 7-8",
	"Playback 9-10", "Playback 11-12", "Playback 13-14", "Playback 15-16" };
static const char *const ozzy_input_pair_names[OZZY_ROUTE_MAX_PAIRS] = {
	"Input 1-2", "Input 3-4", "Input 5-6", "Input 7-8",
	"Input 9-10", "Input 11-12", "Input 13-14", "Input 15-16" };
static const char *const ozzy_pcm_out_frames_names[OZZY_NUM_PCM_OUT_FRAMES] = {
	"10 frames", "20 frames", "30 frames", "40 frames", "50 frames", "60 frames",
	"70 frames", "80 frames", "90 frames", "100 frames", "110 frames", "120 frames",
	"130 frames", "140 frames", "150 frames", "160 frames", "170 frames", "180 frames",
	"190 frames", "200 frames" };
static const char *const ozzy_pcm_in_frames_names[OZZY_NUM_PCM_IN_FRAMES] = {
	"8 frames", "16 frames", "24 frames", "32 frames", "40 frames", "48 frames",
	"56 frames", "64 frames", "72 frames", "80 frames", "88 frames", "96 frames",
	"104 frames", "112 frames", "120 frames", "128 frames", "136 frames", "144 frames",
	"152 frames", "160 frames", "168 frames", "176 frames", "184 frames", "192 frames",
	"200 frames" };

static const char *const ozzy_pcm_queue_names[OZZY_NUM_PCM_QUEUES] = {
	"1 transfer", "2 transfers", "3 transfers", "4 transfers" };
static const char *const ozzy_io_buffer_names[OZZY_NUM_IO_BUFFERS] = {
	"300 frames", "600 frames", "1200 frames", "2400 frames" };
static const char *const ozzy_wm_input_12_names[OZZY_NUM_WM_IN12] = {
	"Mic/Line", "Phono", "Aux", "Sound Module", "Outputs 3-4", "Off" };
static const char *const ozzy_wm_input_34_names[OZZY_NUM_WM_IN34] = {
	"Phono", "Aux", "Sound Module", "Outputs 1-2", "Off" };
static const char *const ozzy_wm_direct_names[OZZY_NUM_WM_DIRECT] = { "Off", "On", "Mono" };
static const char *const ozzy_off_on_names[OZZY_NUM_OFF_ON] = { "Off", "On" };

static const struct ozzy_setting_desc ozzy_setting_descs[OZZY_NUM_SETTINGS] = {
	/* Positional: the macOS kext compiles this as C++11 */
	{ "Clock Source", ozzy_clock_source_names, OZZY_NUM_CLOCK_SOURCES, OZZY_CLOCK_AUTOMATIC,
	  OZZY_QUIRK_DIGITAL_IN, OZZY_SETTING_DEVICE, 0 },
	/* ALSA names S/PDIF "IEC958" (Documentation/sound/designs/control-names.rst) */
	{ "IEC958 Playback Mode", ozzy_spdif_out_mode_names, OZZY_NUM_SPDIF_OUT_MODES,
	  OZZY_SPDIF_OUT_CONSUMER, OZZY_QUIRK_SPDIF_OUT, OZZY_SETTING_DEVICE, 0 },
	{ "Output 1-2 Source", ozzy_playback_pair_names, OZZY_ROUTE_MAX_PAIRS, 0,
	  OZZY_QUIRK_OUT_ROUTING, OZZY_SETTING_OUT_ROUTE, 0 },
	{ "Output 3-4 Source", ozzy_playback_pair_names, OZZY_ROUTE_MAX_PAIRS, 1,
	  OZZY_QUIRK_OUT_ROUTING, OZZY_SETTING_OUT_ROUTE, 1 },
	{ "Capture 1-2 Source", ozzy_input_pair_names, OZZY_ROUTE_MAX_PAIRS, 0,
	  OZZY_QUIRK_IN_ROUTING, OZZY_SETTING_IN_ROUTE, 0 },
	{ "Capture 3-4 Source", ozzy_input_pair_names, OZZY_ROUTE_MAX_PAIRS, 1,
	  OZZY_QUIRK_IN_ROUTING, OZZY_SETTING_IN_ROUTE, 1 },
	{ "Capture 5-6 Source", ozzy_input_pair_names, OZZY_ROUTE_MAX_PAIRS, 2,
	  OZZY_QUIRK_IN_ROUTING, OZZY_SETTING_IN_ROUTE, 2 },
	{ "IEC958 Playback Source", ozzy_playback_pair_names, OZZY_ROUTE_MAX_PAIRS, 0,
	  OZZY_QUIRK_SPDIF_MIRROR, OZZY_SETTING_OUT_PAIR, 0 },
	{ "PCM Playback Transfer", ozzy_pcm_out_frames_names, OZZY_NUM_PCM_OUT_FRAMES, 7,
	  OZZY_QUIRK_PCM_OUT_FRAMES, OZZY_SETTING_DEVICE, 0 },
	{ "PCM Capture Transfer", ozzy_pcm_in_frames_names, OZZY_NUM_PCM_IN_FRAMES, 9,
	  OZZY_QUIRK_PCM_IN_FRAMES, OZZY_SETTING_DEVICE, 0 },
	{ "PCM Playback Queue", ozzy_pcm_queue_names, OZZY_NUM_PCM_QUEUES, OZZY_NUM_PCM_QUEUES - 1,
	  OZZY_QUIRK_PCM_OUT_QUEUE, OZZY_SETTING_DEVICE, 0 },
	{ "PCM Capture Queue", ozzy_pcm_queue_names, OZZY_NUM_PCM_QUEUES, OZZY_NUM_PCM_QUEUES - 1,
	  OZZY_QUIRK_PCM_IN_QUEUE, OZZY_SETTING_DEVICE, 0 },
	{ "Largest IO Buffer", ozzy_io_buffer_names, OZZY_NUM_IO_BUFFERS, 2,
	  OZZY_QUIRK_CLOCK_ANCHORS, OZZY_SETTING_DEVICE, 0 },
	{ "Input 1-2 Source", ozzy_wm_input_12_names, OZZY_NUM_WM_IN12, OZZY_WM_IN12_MIC_LINE,
	  OZZY_QUIRK_WM8776, OZZY_SETTING_DEVICE, 0 },
	{ "Input 3-4 Source", ozzy_wm_input_34_names, OZZY_NUM_WM_IN34, OZZY_WM_IN34_AUX,
	  OZZY_QUIRK_WM8776, OZZY_SETTING_DEVICE, 0 },
	{ "Output 1-2 Playback", ozzy_off_on_names, OZZY_NUM_OFF_ON, 1,
	  OZZY_QUIRK_WM8776, OZZY_SETTING_DEVICE, 0 },
	{ "Output 1-2 Mono", ozzy_off_on_names, OZZY_NUM_OFF_ON, 0,
	  OZZY_QUIRK_WM8776, OZZY_SETTING_DEVICE, 0 },
	{ "Output 1-2 Direct", ozzy_wm_direct_names, OZZY_NUM_WM_DIRECT, OZZY_WM_DIRECT_OFF,
	  OZZY_QUIRK_WM8776, OZZY_SETTING_DEVICE, 0 },
	{ "Output 3-4 Playback", ozzy_off_on_names, OZZY_NUM_OFF_ON, 1,
	  OZZY_QUIRK_WM8776, OZZY_SETTING_DEVICE, 0 },
	{ "Output 3-4 Mono", ozzy_off_on_names, OZZY_NUM_OFF_ON, 0,
	  OZZY_QUIRK_WM8776, OZZY_SETTING_DEVICE, 0 },
	{ "Output 3-4 Direct", ozzy_off_on_names, OZZY_NUM_OFF_ON, 0,
	  OZZY_QUIRK_WM8776, OZZY_SETTING_DEVICE, 0 },
	{ "Output 3-4 Sound Module", ozzy_off_on_names, OZZY_NUM_OFF_ON, 0,
	  OZZY_QUIRK_WM8776, OZZY_SETTING_DEVICE, 0 },
};

/*
 * struct ozzy_settings - What the user chose, one value per enum ozzy_setting.
 * Platforms keep it across device resets and engine restarts.
 */
struct ozzy_settings {
	uint8_t value[OZZY_NUM_SETTINGS];
};

/*
 * struct ozzy_clock_status - What the device reported about its digital input clock,
 * at the last handshake (the official driver only reads it there too).
 * @locked:    clock source Automatic and the device is locked to its S/PDIF input
 * @lock_rate: the rate it locked to (Hz), 0 when not locked
 */
struct ozzy_clock_status {
	bool locked;
	uint32_t lock_rate;
};

static inline void ozzy_settings_defaults(struct ozzy_settings *s)
{
	unsigned int i;

	for (i = 0; i < OZZY_NUM_SETTINGS; i++)
		s->value[i] = ozzy_setting_descs[i].def;
}

/*
 * ozzy_has_setting - Whether a device with @quirks (its ozzy_devices.def row) has setting @id,
 * channel counts aside. Enough for device settings (the handshake uses it); routes also
 * depend on the channels: ozzy_setting_values.
 */
static inline bool ozzy_has_setting(uint32_t quirks, unsigned int id)
{
	return id < OZZY_NUM_SETTINGS && (quirks & ozzy_setting_descs[id].quirk);
}

/*
 * ozzy_setting_count - How many values setting @id has on a device with @in_ch / @out_ch
 * channels that has the setting: a route's values are the pairs on its source side, and a
 * route exists only for a pair the device has (0 then).
 */
static inline unsigned int ozzy_setting_count(unsigned int id, unsigned int in_ch, unsigned int out_ch)
{
	const struct ozzy_setting_desc *d;
	unsigned int pairs;

	if (id >= OZZY_NUM_SETTINGS)
		return 0;
	d = &ozzy_setting_descs[id];
	if (d->kind == OZZY_SETTING_DEVICE)
		return d->num_values;
	pairs = (d->kind == OZZY_SETTING_IN_ROUTE ? in_ch : out_ch) / 2;
	if (pairs > OZZY_ROUTE_MAX_PAIRS)
		pairs = OZZY_ROUTE_MAX_PAIRS;
	return d->pair < pairs ? pairs : 0;   /* OUT_PAIR: pair 0, any playback pair */
}

/*
 * ozzy_setting_values - How many values setting @id has on a device with @quirks and
 * @in_ch / @out_ch channels; 0 if it doesn't have it.
 */
static inline unsigned int ozzy_setting_values(uint32_t quirks, unsigned int in_ch,
					       unsigned int out_ch, unsigned int id)
{
	return ozzy_has_setting(quirks, id) ? ozzy_setting_count(id, in_ch, out_ch) : 0;
}

/*
 * ozzy_settings_route - One direction's routing (@input: capture, else playback) from the
 * route settings in @s, for a device with @channels channels in that direction. Pairs
 * without a route setting, or a device without @available, stay where they are.
 * @available: bit (1 << id) per setting the device has.
 */
static inline void ozzy_settings_route(struct ozzy_route *r, const struct ozzy_settings *s,
				       uint32_t available, bool input, unsigned int channels)
{
	uint8_t src[OZZY_ROUTE_MAX_PAIRS];
	unsigned int id, k;

	for (k = 0; k < OZZY_ROUTE_MAX_PAIRS; k++)
		src[k] = (uint8_t)k;
	for (id = 0; id < OZZY_NUM_SETTINGS; id++) {
		const struct ozzy_setting_desc *d = &ozzy_setting_descs[id];

		if ((available & (1u << id)) &&
		    d->kind == (input ? OZZY_SETTING_IN_ROUTE : OZZY_SETTING_OUT_ROUTE))
			src[d->pair] = s->value[id];
	}
	ozzy_route_set(r, channels, src);
}

#ifdef __cplusplus
}
#endif

#endif /* OZZY_SETTINGS_H */
