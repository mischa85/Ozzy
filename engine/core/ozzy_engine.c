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

/* The routing from the route settings (common/ozzy_settings.h) */
static void update_routes(struct ozzy_engine *e)
{
	uint32_t available = 0;
	unsigned int i;

	for (i = 0; i < OZZY_NUM_SETTINGS; i++)
		if (ozzy_engine_has_setting(e, i))
			available |= 1u << i;
	ozzy_settings_route(&e->in_route, &e->settings, available, true, e->model->in_channels);
	ozzy_settings_route(&e->out_route, &e->settings, available, false, e->model->out_channels);
}

void ozzy_engine_attach(struct ozzy_engine *e, const struct ozzy_bus *bus, volatile struct ozzy_shared_memory *shm)
{
	struct ozzy_engine_memory *m = &e->mem;
	unsigned int i;

	e->bus = bus;
	e->shm = shm;
	shm->magic = OZZY_MAGIC;
	shm->version = OZZY_SHARED_VERSION;
	shm->vendor_id = e->model->vid;
	shm->product_id = e->model->pid;
	if (!shm->product_name[0])
		ozzy_strlcpy((char *)shm->product_name, e->model->name, sizeof(shm->product_name));
	ozzy_settings_defaults(&e->settings);
	update_routes(e);

	/* Transfers in the shared memory: the PCM slots are the HAL's logical packets */
	memset(m, 0, sizeof(*m));
	for (i = 0; i < OZZY_NUM_PACKETS; i++) {
		m->pcm_out[i] = (uint8_t *)shm->audio.output_buffer + i * OZZY_MAX_PACKET_SIZE;
		m->pcm_in[i] = (uint8_t *)shm->audio.input_buffer + i * OZZY_MAX_PACKET_SIZE;
	}
	m->pcm_out_slots = OZZY_NUM_PACKETS;
	m->pcm_in_slots = OZZY_NUM_PACKETS;
	m->isoc_in = (uint8_t *)shm->isoc_in_usb_buffer;
	m->isoc_in_size = sizeof(shm->isoc_in_usb_buffer);
	m->midi_in = (uint8_t *)shm->midi_in_usb_buffer;
	m->midi_out = (uint8_t *)shm->midi_out_usb_buffer;
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
	if (id >= OZZY_NUM_SETTINGS)
		return;
	e->settings.value[id] = value;
	if (ozzy_setting_descs[id].kind != OZZY_SETTING_DEVICE)
		update_routes(e);
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

const struct ozzy_pcm_ring *ozzy_engine_take_pcm_ring(struct ozzy_engine *e, enum ozzy_pcm_dir dir)
{
	const struct ozzy_pcm_ring *ring = (const struct ozzy_pcm_ring *)
		ozzy_load_acquire_ptr((const void *const volatile *)&e->pcm_ring_set[dir]);
	uint32_t channels = dir == OZZY_PCM_OUT ? e->model->out_channels : e->model->in_channels;

	if (ring != e->pcm_ring[dir]) {
		e->pcm_ring[dir] = ring;
		if (ring)
			e->pcm_pos[dir] = ring->start;
	}
	return ring && ring->frames && ring->channels == channels ? ring : NULL;
}
