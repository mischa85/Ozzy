/* SPDX-License-Identifier: MIT */
/*
 * The engine: a device family's protocol on top of a struct ozzy_bus, moving audio and MIDI
 * between USB transfers and the shared memory (engine/shared/ozzy_shared_data.h). One per
 * device. The same code runs in the macOS kext, the Windows driver and the WinUSB harness.
 *
 * A platform:
 *   1. allocates ozzy_engine_size(model) bytes and calls ozzy_engine_init() on them
 *   2. claims the interfaces and endpoints of engine->profile, reporting each pipe it found
 *      with ozzy_engine_set_endpoint()
 *   3. calls ozzy_engine_attach() with its bus and the shared memory, then ozzy_engine_start()
 *   4. hands every completion to ozzy_engine_complete() / ozzy_engine_isoc_complete()
 *   5. calls ozzy_engine_stop(), aborts its pipes, and frees the memory (no destructor)
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_ENGINE_H
#define OZZY_ENGINE_H

#include "ozzy_bus.h"
#include "../shared/ozzy_shared_data.h"
#include "../../common/ozzy_ep.h"
#include "../../common/ozzy_settings.h"
#include "../../common/ozzy_devices.h"
#include "../../common/ozzy_route.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The pipes an engine can ask for */
enum ozzy_pipe_role {
	OZZY_PIPE_PCM_OUT,
	OZZY_PIPE_PCM_IN,
	OZZY_PIPE_MIDI_IN,
	OZZY_PIPE_MIDI_OUT,
	OZZY_PIPE_FEEDBACK,      /* isochronous IN feedback for isochronous PCM out */
	OZZY_PIPE_ROLES,
};

/*
 * struct ozzy_device_profile - What the platform has to claim.
 * @pipes: per role a fixed endpoint address, or (address 0) the first endpoint on its
 *         interface of the given types and direction, the way the official Ploytec driver
 *         finds pipes (common/ozzy_ep.h, as on Linux). All zero: the role is unused.
 */
struct ozzy_device_profile {
	uint8_t interface_count;
	uint8_t interfaces[4];   /* bInterfaceNumber of each */
	uint8_t alt_setting;     /* selected on each claimed interface */
	struct ozzy_ep pipes[OZZY_PIPE_ROLES];
};

/*
 * struct ozzy_pcm_ring - An interleaved S24_3LE ring the engine encodes PCM out from, or
 * decodes PCM in into, in its completion path: where the OS's audio buffer can be reached
 * from the kernel (the Windows driver's WaveRT buffer). Without one (macOS: the HAL writes
 * the shared memory rings itself) the engine sends and receives the shared memory as it is.
 * @channels: the model's channel count in that direction
 * @start:    ring position of the next packet's first frame (frame @start % @frames)
 */
struct ozzy_pcm_ring {
	uint8_t *base;
	uint32_t frames;
	uint32_t channels;
	uint64_t start;
};

struct ozzy_engine;

/*
 * struct ozzy_engine_ops - One per device family. Optional: @suspend, @isoc_complete,
 * @midi_out_pending, @timer, @setting_quirks.
 */
struct ozzy_engine_ops {
	void (*set_endpoint)(struct ozzy_engine *e, enum ozzy_pipe_role role, uint8_t address,
			     uint8_t transfer_type, uint16_t max_packet_size, uint8_t interval);
	bool (*start)(struct ozzy_engine *e);
	void (*stop)(struct ozzy_engine *e);
	void (*suspend)(struct ozzy_engine *e);
	void (*complete)(struct ozzy_engine *e, uint8_t pipe, void *xfer, int status, uint32_t bytes);
	void (*isoc_complete)(struct ozzy_engine *e, uint8_t pipe, void *xfer, int status,
			      struct ozzy_isoc_packet *packets, uint32_t count);
	void (*midi_out_pending)(struct ozzy_engine *e);
	void (*timer)(struct ozzy_engine *e);
	uint32_t (*setting_quirks)(const struct ozzy_engine *e);
};

/*
 * struct ozzy_engine - What every family has, the first member of its own struct.
 * @settings:     what start() sends (ozzy_engine_attach: the defaults)
 * @in_route/@out_route: the route settings' routing, which the ring path applies (on macOS
 *                the HAL routes); updated from ozzy_engine_set_setting() without a lock, as
 *                on Linux: a packet during a change may mix the old and new routing
 * @pcm_ring_set: ozzy_engine_set_pcm_ring(); the completion path picks it up
 */
struct ozzy_engine {
	const struct ozzy_engine_ops *ops;
	const struct ozzy_bus *bus;
	volatile struct ozzy_shared_memory *shm;
	const struct ozzy_device *model;
	struct ozzy_device_profile profile;
	struct ozzy_settings settings;
	struct ozzy_route in_route;
	struct ozzy_route out_route;
	const struct ozzy_pcm_ring *volatile pcm_ring_set[OZZY_PCM_DIRS];
};

/* Bytes to allocate for @model's engine (0 if Ozzy has none for its family) */
size_t ozzy_engine_size(const struct ozzy_device *model);
/* The engine for @model in @mem (ozzy_engine_size() bytes, any contents), or NULL */
struct ozzy_engine *ozzy_engine_init(void *mem, const struct ozzy_device *model);
/* The bus and shared memory to run on; the settings go back to their defaults */
void ozzy_engine_attach(struct ozzy_engine *e, const struct ozzy_bus *bus, volatile struct ozzy_shared_memory *shm);

static inline void ozzy_engine_set_endpoint(struct ozzy_engine *e, enum ozzy_pipe_role role, uint8_t address,
					    uint8_t transfer_type, uint16_t max_packet_size, uint8_t interval)
{
	e->ops->set_endpoint(e, role, address, transfer_type, max_packet_size, interval);
}

static inline bool ozzy_engine_start(struct ozzy_engine *e)
{
	return e->ops->start(e);
}

static inline void ozzy_engine_stop(struct ozzy_engine *e)
{
	e->ops->stop(e);
}

/*
 * Restart for changed settings: stop resubmitting transfers and the timer; the platform then
 * aborts every pipe and calls ozzy_engine_start() again. Unlike stop it keeps hardware_present
 * and driver_ready, so the HAL keeps the device published (CoreAudio has stopped IO).
 */
static inline void ozzy_engine_suspend(struct ozzy_engine *e)
{
	if (e->ops->suspend)
		e->ops->suspend(e);
}

/* A transfer completed. Transfer type bits as bmAttributes & 3. */
static inline void ozzy_engine_complete(struct ozzy_engine *e, uint8_t pipe, void *xfer, int status, uint32_t bytes)
{
	e->ops->complete(e, pipe, xfer, status, bytes);
}

static inline void ozzy_engine_isoc_complete(struct ozzy_engine *e, uint8_t pipe, void *xfer, int status,
					     struct ozzy_isoc_packet *packets, uint32_t count)
{
	if (e->ops->isoc_complete)
		e->ops->isoc_complete(e, pipe, xfer, status, packets, count);
}

/* The MIDI client queued bytes in midi_out. Engines that send MIDI on their own schedule
 * (Ploytec embeds it in every PCM packet) ignore this. */
static inline void ozzy_engine_midi_out_pending(struct ozzy_engine *e)
{
	if (e->ops->midi_out_pending)
		e->ops->midi_out_pending(e);
}

/* The bus timer (ozzy_bus_ops.start_timer) fired */
static inline void ozzy_engine_timer(struct ozzy_engine *e)
{
	if (e->ops->timer)
		e->ops->timer(e);
}

/* Attach @ring for @dir (NULL detaches). The completion path picks it up with the next
 * packet. The ring must stay valid until the caller has waited out the completions in
 * progress after detaching. */
static inline void ozzy_engine_set_pcm_ring(struct ozzy_engine *e, enum ozzy_pcm_dir dir,
					    const struct ozzy_pcm_ring *ring)
{
	ozzy_store_release_ptr((const void *volatile *)&e->pcm_ring_set[dir], ring);
}

/* --- Device settings (common/ozzy_settings.h) --- */

/* The model's quirks as far as settings go (a family drops what its transport can't do) */
uint32_t ozzy_engine_setting_quirks(const struct ozzy_engine *e);
bool ozzy_engine_has_setting(const struct ozzy_engine *e, unsigned int id);
/* A device setting takes effect with the next start (ozzy_engine_suspend first); a route
 * setting at once */
void ozzy_engine_set_setting(struct ozzy_engine *e, unsigned int id, uint8_t value);

/* For the families: publish @e->settings and the clock status of the handshake start() just ran */
void ozzy_engine_publish_settings(struct ozzy_engine *e, const struct ozzy_clock_status *clock);

#ifdef __cplusplus
}
#endif

#endif /* OZZY_ENGINE_H */
