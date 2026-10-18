// SPDX-License-Identifier: MIT
/*
 * ozzy: the settings app, a libui-ng window on the shared control core (control/ozzy_control.h).
 *
 * One window for one device at a time (a picker when there are more). Everything known about
 * the device is in one place, in sections: what can be changed sits with what the driver says
 * about it (the clock source with the rates and the digital input lock), the rest of the facts
 * (ozzy_control_facts, as ozzy-cli show prints them) in sections of their own. Two columns, so
 * the window fits a laptop screen. The window follows the backend's change notifications: a
 * device coming or going, or a setting changed here or anywhere else, shows at once. Nothing
 * polls.
 *
 * libui-ng runs on the main thread only. Notifications arrive on a backend thread and only
 * queue a refresh there (uiQueueMain); a setting is applied on a thread of its own, since a
 * device setting restarts the device (about a second) and the window must not hang meanwhile.
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
#endif

#include "ui.h"
#include "../control/ozzy_control.h"

/* The window's sections, in order: the first GUI_LEFT_SECTIONS in the left column */
enum gui_section {
	GUI_SECTION_DEVICE,
	GUI_SECTION_AUDIO,
	GUI_SECTION_DIGITAL_OUT,
	GUI_SECTION_ROUTING,
	GUI_SECTION_STREAM,
	GUI_SECTION_ENDPOINTS,
	GUI_SECTION_HOST,
	GUI_NUM_SECTIONS
};

#define GUI_LEFT_SECTIONS GUI_SECTION_STREAM

static const char *const gui_section_titles[GUI_NUM_SECTIONS] = {
	"Device", "Audio", "Digital output", "Routing", "Stream", "Endpoints", "Operating system"
};

/* Where each group of facts goes */
static const enum gui_section gui_fact_sections[OZZY_CONTROL_GROUPS] = {
	[OZZY_CONTROL_GROUP_DEVICE]    = GUI_SECTION_DEVICE,
	[OZZY_CONTROL_GROUP_AUDIO]     = GUI_SECTION_AUDIO,
	[OZZY_CONTROL_GROUP_STREAM]    = GUI_SECTION_STREAM,
	[OZZY_CONTROL_GROUP_ENDPOINTS] = GUI_SECTION_ENDPOINTS,
	[OZZY_CONTROL_GROUP_HOST]      = GUI_SECTION_HOST,
};

/* What the window calls a setting, and its section (settings come before the facts there) */
static const struct {
	enum gui_section section;
	const char *label;
} gui_settings[OZZY_NUM_SETTINGS] = {
	[OZZY_SETTING_CLOCK_SOURCE]   = { GUI_SECTION_AUDIO, "Clock source" },
	[OZZY_SETTING_SPDIF_OUT_MODE] = { GUI_SECTION_DIGITAL_OUT, "S/PDIF format" },
	[OZZY_SETTING_SPDIF_MIRROR]   = { GUI_SECTION_DIGITAL_OUT, "S/PDIF plays" },
	[OZZY_SETTING_OUT_ROUTE_1]    = { GUI_SECTION_ROUTING, "Outputs 1-2 play" },
	[OZZY_SETTING_OUT_ROUTE_2]    = { GUI_SECTION_ROUTING, "Outputs 3-4 play" },
	[OZZY_SETTING_IN_ROUTE_1]     = { GUI_SECTION_ROUTING, "Capture 1-2 records" },
	[OZZY_SETTING_IN_ROUTE_2]     = { GUI_SECTION_ROUTING, "Capture 3-4 records" },
	[OZZY_SETTING_IN_ROUTE_3]     = { GUI_SECTION_ROUTING, "Capture 5-6 records" },
	[OZZY_SETTING_PCM_OUT_FRAMES] = { GUI_SECTION_STREAM, "Out transfer" },
	[OZZY_SETTING_PCM_IN_FRAMES]  = { GUI_SECTION_STREAM, "In transfer" },
	[OZZY_SETTING_PCM_OUT_QUEUE]  = { GUI_SECTION_STREAM, "Out queue" },
	[OZZY_SETTING_PCM_IN_QUEUE]   = { GUI_SECTION_STREAM, "In queue" },
};

/* One setting's row: its combobox */
struct gui_row {
	unsigned int id;
	uiCombobox *box;
};

/*
 * The window. @content is built for one device's shape (gui_same_shape: the device, its
 * settings and which facts it has) and rebuilt when that changes; values are updated in place
 * otherwise.
 */
static struct {
	uiWindow *win;
	uiBox *root;
	uiBox *content;                      /* NULL before the first refresh */
	uiCombobox *picker;                  /* NULL with one device or none */
	uiLabel *note;
	struct gui_row rows[OZZY_NUM_SETTINGS];
	unsigned int nrows;
	struct ozzy_control_fact facts[OZZY_CONTROL_MAX_FACTS];   /* as the window shows them */
	uiLabel *fact_values[OZZY_CONTROL_MAX_FACTS];
	int nfacts;
	struct ozzy_control_device devs[OZZY_CONTROL_MAX_DEVICES];
	int ndevs;                           /* -1: the backend can't list */
	int current;                         /* index into devs, -1 if none */
	char current_id[160];                /* the device shown, kept across refreshes */
	bool busy;                           /* a device setting is being applied */
	bool rebuild;                        /* the next refresh builds the window anew */
	bool closing;                        /* the window is gone: queued work does nothing */
} gui;

/* A setting to apply on its own thread; @device: a device setting, which made the window busy */
struct gui_apply {
	struct ozzy_control_device dev;
	unsigned int id;
	unsigned int value;
	bool device;
	int result;
};

static void gui_refresh(void);
static void gui_on_change_main(void *data);

/* --- Threads: a setting off the main thread --- */

#ifdef _WIN32
static DWORD WINAPI gui_apply_thread(LPVOID arg);
static bool gui_start_thread(struct gui_apply *a)
{
	HANDLE t = CreateThread(NULL, 0, gui_apply_thread, a, 0, NULL);

	if (!t)
		return false;
	CloseHandle(t);
	return true;
}
#else
static void *gui_apply_thread(void *arg);
static bool gui_start_thread(struct gui_apply *a)
{
	pthread_t t;

	if (pthread_create(&t, NULL, gui_apply_thread, a))
		return false;
	pthread_detach(t);
	return true;
}
#endif

/* --- Applying a setting --- */

/* The line under the sections: what a change does, or that one is under way */
static void gui_note(void)
{
	const struct ozzy_control_device *d = gui.current >= 0 ? &gui.devs[gui.current] : NULL;
	bool device_settings = false;
	unsigned int i;

	if (!gui.note || !d)
		return;
	for (i = 0; i < gui.nrows; i++)
		device_settings |= ozzy_setting_descs[gui.rows[i].id].kind == OZZY_SETTING_DEVICE;
	uiLabelSetText(gui.note, gui.busy ? "Applying; the device's audio restarts for a moment..." :
			       !gui.nrows ? "This device has no settings." :
			       device_settings ? "Clock, S/PDIF and transfer changes restart the device's audio for a moment." : "");
}

static void gui_set_enabled(bool enabled)
{
	unsigned int i;

	for (i = 0; i < gui.nrows; i++) {
		if (enabled)
			uiControlEnable(uiControl(gui.rows[i].box));
		else
			uiControlDisable(uiControl(gui.rows[i].box));
	}
	if (gui.picker) {
		if (enabled)
			uiControlEnable(uiControl(gui.picker));
		else
			uiControlDisable(uiControl(gui.picker));
	}
}

static void gui_apply_done(void *arg)
{
	struct gui_apply *a = arg;

	if (gui.closing) {
		free(a);
		return;
	}
	/* Only the device setting that made the window busy ends it (a route may finish meanwhile) */
	if (a->device)
		gui.busy = false;
	if (a->result < 0)
		uiMsgBoxError(gui.win, "The setting didn't take",
			      "The device didn't accept it. Is it still connected?");
	free(a);
	gui_refresh();   /* the device's own values, whatever happened */
}

#ifdef _WIN32
static DWORD WINAPI gui_apply_thread(LPVOID arg)
#else
static void *gui_apply_thread(void *arg)
#endif
{
	struct gui_apply *a = arg;

	a->result = ozzy_control_backend_set(&a->dev, a->id, a->value);
	uiQueueMain(gui_apply_done, a);
#ifdef _WIN32
	return 0;
#else
	return NULL;
#endif
}

static void gui_on_setting(uiCombobox *box, void *data)
{
	struct gui_row *row = data;
	const struct ozzy_control_device *d;
	struct gui_apply *a;
	int v = uiComboboxSelected(box);

	if (gui.current < 0 || gui.busy || v < 0)
		return;
	d = &gui.devs[gui.current];
	if ((unsigned int)v == d->settings.value[row->id])
		return;
	a = calloc(1, sizeof(*a));
	if (!a)
		return;
	a->dev = *d;
	a->id = row->id;
	a->value = (unsigned int)v;

	/* A route applies at once; a device setting restarts the device: no second one meanwhile */
	a->device = ozzy_setting_descs[row->id].kind == OZZY_SETTING_DEVICE;
	if (a->device) {
		gui.busy = true;
		gui_set_enabled(false);
		gui_note();
	}
	/* Not done here if there is no thread: the refresh may destroy this combobox */
	if (!gui_start_thread(a)) {
		a->result = -1;
		uiQueueMain(gui_apply_done, a);
	}
}

/* --- Building the window --- */

static void gui_on_pick(uiCombobox *box, void *data)
{
	int i = uiComboboxSelected(box);

	(void)data;
	if (i < 0 || i >= gui.ndevs || i == gui.current)
		return;
	snprintf(gui.current_id, sizeof(gui.current_id), "%s", gui.devs[i].id);
	/* Not from inside the picker's own callback: the rebuild destroys the picker */
	gui.rebuild = true;
	uiQueueMain(gui_on_change_main, NULL);
}

/* A combobox for setting @id of @d, with its values, in @form */
static void gui_add_setting(uiForm *form, const struct ozzy_control_device *d, unsigned int id)
{
	struct gui_row *row = &gui.rows[gui.nrows++];
	unsigned int n = ozzy_control_values(d, id), v;

	row->id = id;
	row->box = uiNewCombobox();
	for (v = 0; v < n; v++)
		uiComboboxAppend(row->box, ozzy_setting_descs[id].value_names[v]);
	uiComboboxOnSelected(row->box, gui_on_setting, row);
	uiFormAppend(form, gui_settings[id].label, uiControl(row->box), 0);
}

static uiBox *gui_build_empty(const char *text)
{
	uiBox *box = uiNewVerticalBox();

	uiBoxSetPadded(box, 1);
	uiBoxAppend(box, uiControl(uiNewLabel(text)), 0);
	return box;
}

/* Section @s: its settings, then its facts; NULL if it has neither */
static uiGroup *gui_build_section(const struct ozzy_control_device *d, enum gui_section s)
{
	uiGroup *group = NULL;
	uiForm *form = NULL;
	unsigned int id;
	int i;

	for (id = 0; id <= OZZY_NUM_SETTINGS; id++) {
		bool setting = id < OZZY_NUM_SETTINGS;

		/* id == OZZY_NUM_SETTINGS: the facts, once */
		if (setting && (gui_settings[id].section != s || !ozzy_control_values(d, id)))
			continue;
		for (i = 0; i < (setting ? 1 : gui.nfacts); i++) {
			if (!setting && gui_fact_sections[gui.facts[i].group] != s)
				continue;
			if (!group) {
				group = uiNewGroup(gui_section_titles[s]);
				uiGroupSetMargined(group, 1);
				form = uiNewForm();
				uiFormSetPadded(form, 1);
				uiGroupSetChild(group, uiControl(form));
			}
			if (setting) {
				gui_add_setting(form, d, id);
			} else {
				gui.fact_values[i] = uiNewLabel(gui.facts[i].value);
				uiFormAppend(form, gui.facts[i].label, uiControl(gui.fact_values[i]), 0);
			}
		}
	}
	return group;
}

static uiBox *gui_build(void)
{
	const struct ozzy_control_device *d = &gui.devs[gui.current];
	uiBox *box = uiNewVerticalBox();
	uiBox *columns = uiNewHorizontalBox();
	uiBox *column[2] = { uiNewVerticalBox(), uiNewVerticalBox() };
	unsigned int s;
	int i;

	uiBoxSetPadded(box, 1);
	uiBoxSetPadded(columns, 1);
	gui.nrows = 0;
	gui.picker = NULL;

	if (gui.ndevs > 1) {
		uiForm *form = uiNewForm();

		uiFormSetPadded(form, 1);
		gui.picker = uiNewCombobox();
		for (i = 0; i < gui.ndevs; i++)
			uiComboboxAppend(gui.picker, gui.devs[i].name);
		uiComboboxSetSelected(gui.picker, gui.current);
		uiComboboxOnSelected(gui.picker, gui_on_pick, NULL);
		uiFormAppend(form, "Device", uiControl(gui.picker), 0);
		uiBoxAppend(box, uiControl(form), 0);
	} else {
		uiBoxAppend(box, uiControl(uiNewLabel(d->name)), 0);
	}

	for (s = 0; s < GUI_NUM_SECTIONS; s++) {
		uiGroup *group = gui_build_section(d, (enum gui_section)s);

		if (group)
			uiBoxAppend(column[s >= GUI_LEFT_SECTIONS], uiControl(group), 0);
	}
	uiBoxSetPadded(column[0], 1);
	uiBoxSetPadded(column[1], 1);
	uiBoxAppend(columns, uiControl(column[0]), 1);
	uiBoxAppend(columns, uiControl(column[1]), 1);
	uiBoxAppend(box, uiControl(columns), 1);

	gui.note = uiNewLabel("");
	uiBoxAppend(box, uiControl(gui.note), 0);
	return box;
}

/* Whether the window built for @a fits @b: same device, settings and channels */
static bool gui_same_shape(const struct ozzy_control_device *a, const struct ozzy_control_device *b)
{
	return !strcmp(a->id, b->id) && !strcmp(a->name, b->name) && a->available == b->available &&
	       a->in_channels == b->in_channels && a->out_channels == b->out_channels &&
	       a->has_clock == b->has_clock;
}

/* Whether @facts (@n) are the ones the window has, labels and groups, values aside */
static bool gui_same_facts(const struct ozzy_control_fact *facts, int n)
{
	int i;

	if (n != gui.nfacts)
		return false;
	for (i = 0; i < n; i++)
		if (facts[i].group != gui.facts[i].group || strcmp(facts[i].label, gui.facts[i].label))
			return false;
	return true;
}

/* The values of the device shown */
static void gui_update(void)
{
	const struct ozzy_control_device *d = &gui.devs[gui.current];
	unsigned int i;
	int k;

	for (i = 0; i < gui.nrows; i++)
		uiComboboxSetSelected(gui.rows[i].box, d->settings.value[gui.rows[i].id]);
	for (k = 0; k < gui.nfacts; k++)
		if (gui.fact_values[k])
			uiLabelSetText(gui.fact_values[k], gui.facts[k].value);
	gui_note();
	gui_set_enabled(!gui.busy);
}

/* List the devices again and show what changed: rebuilt if the shape did, else in place */
static void gui_refresh(void)
{
	struct ozzy_control_device old[OZZY_CONTROL_MAX_DEVICES];
	struct ozzy_control_fact facts[OZZY_CONTROL_MAX_FACTS];
	int nold = gui.ndevs, oldcur = gui.current, nfacts = 0, i;
	bool rebuild;

	memcpy(old, gui.devs, sizeof(old));
	gui.ndevs = ozzy_control_backend_list(gui.devs, OZZY_CONTROL_MAX_DEVICES);
	gui.current = -1;
	for (i = 0; i < gui.ndevs; i++)
		if (!strcmp(gui.devs[i].id, gui.current_id))
			gui.current = i;
	if (gui.current < 0 && gui.ndevs > 0) {
		gui.current = 0;
		snprintf(gui.current_id, sizeof(gui.current_id), "%s", gui.devs[0].id);
	}
	if (gui.current >= 0)
		nfacts = ozzy_control_facts(&gui.devs[gui.current], facts, OZZY_CONTROL_MAX_FACTS);

	rebuild = gui.rebuild || !gui.content || gui.ndevs != nold || gui.current != oldcur ||
		  (gui.current >= 0 && !gui_same_shape(&gui.devs[gui.current], &old[oldcur])) ||
		  !gui_same_facts(facts, nfacts);
	for (i = 0; !rebuild && gui.picker && i < gui.ndevs; i++)
		rebuild = strcmp(gui.devs[i].name, old[i].name) != 0;
	memcpy(gui.facts, facts, sizeof(facts[0]) * (size_t)nfacts);
	gui.nfacts = nfacts;
	if (rebuild) {
		gui.rebuild = false;
		if (gui.content) {
			uiBoxDelete(gui.root, 0);
			uiControlDestroy(uiControl(gui.content));
		}
		gui.nrows = 0;
		gui.picker = NULL;
		gui.note = NULL;
		memset(gui.fact_values, 0, sizeof(gui.fact_values));
		if (gui.ndevs < 0)
			gui.content = gui_build_empty("Can't look for Ozzy devices: is the Ozzy driver installed?");
		else if (gui.current < 0)
			gui.content = gui_build_empty("No Ozzy device connected.\nConnect one and it shows up here.");
		else
			gui.content = gui_build();
		uiBoxAppend(gui.root, uiControl(gui.content), 1);
	}
	if (gui.current >= 0)
		gui_update();
}

/* --- Notifications and the main loop --- */

static void gui_on_change_main(void *data)
{
	(void)data;
	if (!gui.closing)
		gui_refresh();
}

/* Backend thread: only hand it to the main thread */
static void gui_on_change(void *ctx)
{
	(void)ctx;
	uiQueueMain(gui_on_change_main, NULL);
}

/* The window goes: what is still queued (a notification, an apply finishing) must not touch it */
static int gui_on_closing(uiWindow *w, void *data)
{
	(void)w;
	(void)data;
	gui.closing = true;
	uiQuit();
	return 1;
}

static int gui_on_should_quit(void *data)
{
	(void)data;
	gui.closing = true;
	uiControlDestroy(uiControl(gui.win));
	return 1;
}

int main(void)
{
	uiInitOptions options;
	const char *err;

	memset(&options, 0, sizeof(options));
	err = uiInit(&options);
	if (err) {
		fprintf(stderr, "ozzy: can't start the window system: %s\n", err);
		uiFreeInitError(err);
		return 1;
	}

	gui.win = uiNewWindow("Ozzy", 860, 560, 0);
	uiWindowSetMargined(gui.win, 1);
	uiWindowOnClosing(gui.win, gui_on_closing, NULL);
	uiOnShouldQuit(gui_on_should_quit, NULL);
	gui.root = uiNewVerticalBox();
	uiBoxSetPadded(gui.root, 1);
	uiWindowSetChild(gui.win, uiControl(gui.root));
	gui.ndevs = 0;
	gui.current = -1;

	/* Watching first, so nothing that changes while the window comes up is missed */
	if (ozzy_control_backend_watch(gui_on_change, NULL) < 0)
		fprintf(stderr, "ozzy: no change notifications; the window shows what was there at start\n");
	gui_refresh();

	uiControlShow(uiControl(gui.win));
	uiMain();
	uiUninit();
	return 0;
}
