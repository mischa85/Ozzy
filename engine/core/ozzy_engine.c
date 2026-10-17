/* SPDX-License-Identifier: MIT */
/*
 * The engine: one factory for every platform, and what all families share.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "ozzy_engine.h"
#include "../shared/ozzy_log.h"
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

	ozzy_engine_free_memory(e);
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

void ozzy_engine_bind_endpoints(struct ozzy_engine *e, const struct ozzy_usb_endpoint *eps, unsigned int count,
				const struct ozzy_usb_endpoint *bound[OZZY_PIPE_ROLES])
{
	static const char *const types[] = { "control", "isoc", "bulk", "interrupt" };
	unsigned int role, i;

	for (i = 0; i < count; i++)
		ozzy_log("  interface %u: EP 0x%02x %-3s %-9s wMaxPacketSize %u x%u bInterval %u", eps[i].intf,
			 eps[i].address, (eps[i].address & 0x80) ? "IN" : "OUT", types[eps[i].attributes & 3],
			 eps[i].max_packet & 0x07FF, ((eps[i].max_packet >> 11) & 3) + 1, eps[i].interval);

	for (role = 0; role < OZZY_PIPE_ROLES; role++) {
		const struct ozzy_ep *spec = &e->profile.pipes[role];
		const struct ozzy_usb_endpoint *ep = NULL;

		bound[role] = NULL;
		if (!spec->address && !spec->types)
			continue;
		for (i = 0; i < count && !ep; i++) {
			if (eps[i].intf != spec->intf)
				continue;
			if (spec->address ? eps[i].address == spec->address :
					    ozzy_ep_matches(spec, eps[i].address, eps[i].attributes))
				ep = &eps[i];
		}
		if (!ep) {
			ozzy_log("  no endpoint for pipe role %u on interface %u", role, spec->intf);
			continue;
		}
		bound[role] = ep;
		ozzy_engine_set_endpoint(e, (enum ozzy_pipe_role)role, ep->address, ep->attributes & 3,
					 ozzy_usb_endpoint_bytes(ep), ep->interval);
	}
}

static uint8_t *alloc_zeroed(const struct ozzy_allocator *a, size_t size, bool *ok)
{
	uint8_t *p = (uint8_t *)a->alloc(a->ctx, size);

	if (p)
		memset(p, 0, size);
	else
		*ok = false;
	return p;
}

bool ozzy_engine_alloc_memory(struct ozzy_engine *e, const struct ozzy_allocator *a)
{
	const struct ozzy_engine_pipe *out = &e->pipe[OZZY_PIPE_PCM_OUT];
	const struct ozzy_engine_pipe *fb = &e->pipe[OZZY_PIPE_FEEDBACK];
	uint32_t fb_bytes = fb->transfer_type == 1 && fb->max_packet_size > 4 ? fb->max_packet_size : 4;
	struct ozzy_engine_memory *m = &e->mem;
	bool ok = true;
	unsigned int i;

	ozzy_engine_free_memory(e);
	memset(m, 0, sizeof(*m));
	e->allocator = a;
	for (i = 0; i < OZZY_PCM_OWN_SLOTS; i++) {
		m->pcm_out[i] = alloc_zeroed(a, OZZY_PCM_SLOT_MIN, &ok);
		m->pcm_in[i] = alloc_zeroed(a, OZZY_PCM_SLOT_MIN, &ok);
	}
	m->pcm_out_slots = m->pcm_in_slots = OZZY_PCM_OWN_SLOTS;
	if (out->transfer_type == 1 && out->max_packet_size) {
		/* One OUT transfer: at most a packet per microframe, each at most the bytes per interval */
		m->isoc_out_size = OZZY_DMA_ROUND(OZZY_MAX_ISOC_PACKETS * out->max_packet_size);
		for (i = 0; i < ISOC_OUT_TRANSFERS; i++)
			m->isoc_out[i] = alloc_zeroed(a, m->isoc_out_size, &ok);
	}
	m->isoc_in_size = ISOC_FB_TRANSFERS * OZZY_DMA_ROUND(OZZY_MAX_ISOC_PACKETS * fb_bytes);
	m->isoc_in = alloc_zeroed(a, m->isoc_in_size, &ok);
	m->midi_in = alloc_zeroed(a, OZZY_MIDI_IN_SIZE, &ok);
	m->midi_out = alloc_zeroed(a, OZZY_DMA_ROUND(OZZY_MIDI_OUT_SIZE), &ok);
	if (!ok)
		ozzy_engine_free_memory(e);
	return ok;
}

void ozzy_engine_free_memory(struct ozzy_engine *e)
{
	const struct ozzy_allocator *a = e->allocator;
	struct ozzy_engine_memory *m = &e->mem;
	unsigned int i;

	if (!a)
		return;
	for (i = 0; i < OZZY_NUM_PACKETS; i++) {
		if (m->pcm_out[i])
			a->free(a->ctx, m->pcm_out[i]);
		if (m->pcm_in[i])
			a->free(a->ctx, m->pcm_in[i]);
	}
	for (i = 0; i < ISOC_OUT_TRANSFERS; i++)
		if (m->isoc_out[i])
			a->free(a->ctx, m->isoc_out[i]);
	if (m->isoc_in)
		a->free(a->ctx, m->isoc_in);
	if (m->midi_in)
		a->free(a->ctx, m->midi_in);
	if (m->midi_out)
		a->free(a->ctx, m->midi_out);
	memset(m, 0, sizeof(*m));
	e->allocator = NULL;
}
