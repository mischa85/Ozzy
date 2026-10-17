/* SPDX-License-Identifier: MIT */
/*
 * The engine: one factory for every platform, and what all families share.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "ozzy_engine.h"
#include "../devices/ploytec/ploytec_engine.h"
#include "../devices/ploytec/ploytec_isoc_engine.h"
#include "../devices/usbmidi/usb_midi_engine.h"
#include <string.h>

size_t ozzy_engine_size(const struct ozzy_device *model)
{
	switch (model->family) {
	case OZZY_FAMILY_PLOYTEC_XONE:  return sizeof(struct ploytec_engine);
	case OZZY_FAMILY_PLOYTEC_ISOC:  return sizeof(struct ploytec_isoc_engine);
	case OZZY_FAMILY_USB_MIDI:      return sizeof(struct usb_midi_engine);
	default:                        return 0;
	}
}

struct ozzy_engine *ozzy_engine_init(void *mem, const struct ozzy_device *model)
{
	size_t size = ozzy_engine_size(model);

	if (!mem || !size)
		return NULL;
	memset(mem, 0, size);
	switch (model->family) {
	case OZZY_FAMILY_PLOYTEC_XONE:
		ploytec_engine_init((struct ploytec_engine *)mem, model);
		break;
	case OZZY_FAMILY_PLOYTEC_ISOC:
		ploytec_isoc_engine_init((struct ploytec_isoc_engine *)mem, model);
		break;
	case OZZY_FAMILY_USB_MIDI:
		usb_midi_engine_init((struct usb_midi_engine *)mem, model);
		break;
	}
	return (struct ozzy_engine *)mem;
}

void ozzy_engine_attach(struct ozzy_engine *e, const struct ozzy_bus *bus, volatile struct ozzy_shared_memory *shm)
{
	e->bus = bus;
	e->shm = shm;
	ozzy_settings_defaults(&e->settings);
}

uint32_t ozzy_engine_setting_quirks(const struct ozzy_engine *e)
{
	return e->ops->setting_quirks ? e->ops->setting_quirks(e) : e->model->quirks;
}

bool ozzy_engine_has_setting(const struct ozzy_engine *e, unsigned int id)
{
	return ozzy_setting_values(ozzy_engine_setting_quirks(e), e->model->in_channels,
				   e->model->out_channels, id) > 0;
}

void ozzy_engine_set_setting(struct ozzy_engine *e, unsigned int id, uint8_t value)
{
	if (id < OZZY_NUM_SETTINGS)
		e->settings.value[id] = value;
}

void ozzy_engine_publish_settings(struct ozzy_engine *e, const struct ozzy_clock_status *clock)
{
	volatile struct ozzy_shared_memory *shm = e->shm;
	uint32_t available = 0;
	unsigned int i;

	if (!shm)
		return;
	for (i = 0; i < OZZY_NUM_SETTINGS; i++) {
		if (ozzy_engine_has_setting(e, i))
			available |= 1u << i;
		shm->audio.settings.value[i] = e->settings.value[i];
	}
	shm->audio.settings.available = available;
	shm->audio.settings.lock_rate = clock->locked ? clock->lock_rate : 0;
	shm->audio.settings.clock_locked = clock->locked;
}
