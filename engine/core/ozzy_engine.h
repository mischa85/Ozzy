/* SPDX-License-Identifier: MIT */
/*
 * The engine: a device family's protocol on top of a struct ozzy_bus, moving audio and MIDI
 * between USB transfers and the shared memory (engine/shared/ozzy_shared_data.h). One per
 * device. The same code runs in the Linux module, the macOS kext, the Windows driver and the
 * WinUSB harness.
 *
 * A platform:
 *   1. allocates ozzy_engine_size(model) bytes and calls ozzy_engine_init() on them
 *   2. claims the interfaces of engine->profile at its alternate setting, lists their
 *      endpoints for ozzy_engine_bind_endpoints() and opens the pipes it picks
 *   3. calls ozzy_engine_attach() with its bus and the shared memory, then ozzy_engine_start()
 *   4. hands every completion to ozzy_engine_complete() / ozzy_engine_isoc_complete()
 *   5. calls ozzy_engine_stop(), which aborts every transfer (ozzy_bus_ops.abort), and frees
 *      the memory (no destructor)
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
#include "../../common/isoc_feedback.h"

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

/*
 * For pcm_progress: how many boundaries every @step frames (ALSA periods, WaveRT packets) ring
 * position @pos has reached since @next, the next one; @next moves past them.
 */
static inline uint32_t ozzy_pcm_boundaries(uint64_t *next, uint64_t pos, uint32_t step)
{
	uint32_t n = 0;

	while (step && pos >= *next) {
		*next += step;
		n++;
	}
	return n;
}

/*
 * struct ozzy_engine_memory - Where the engine's transfers go: every buffer it hands to
 * ozzy_bus_ops.submit / submit_isoc is in here. ozzy_engine_attach() points it into the
 * shared memory (macOS: the HAL reads and writes the PCM slots there; Windows, the harness and
 * the tests use it as it is). A platform whose transfers need memory of their own (Linux:
 * DMA-capable, a cache line per buffer) has the engine allocate it (ozzy_engine_alloc_memory).
 * @pcm_out/@pcm_in:  slots of at least OZZY_PCM_SLOT_MIN bytes; transfer idx uses slot
 *                    idx % slots (at least the transfers in flight, 4)
 * @isoc_out:         NULL: isochronous OUT packets point into the shared memory's frame ring,
 *                    which the HAL writes; else one buffer per OUT transfer
 *                    (ISOC_OUT_TRANSFERS, @isoc_out_size bytes each), filled at submit
 * @isoc_in:          the feedback transfers (@isoc_in_size bytes)
 * @midi_in:          MIDI IN transfers, OZZY_MIDI_IN_SIZE bytes (OZZY_DMA_ALIGN aligned)
 * @midi_out:         one MIDI OUT transfer, OZZY_MIDI_OUT_SIZE bytes
 */
#define OZZY_PCM_SLOT_MIN   5120   /* the largest PCM transfer (Ploytec PCM in) */
#define OZZY_MIDI_IN_SIZE   512    /* 4 transfers in flight, OZZY_DMA_ALIGN apart */
#define OZZY_MIDI_OUT_SIZE  64
#define OZZY_PCM_OWN_SLOTS  4      /* ozzy_engine_alloc_memory: PCM buffers per direction */

struct ozzy_engine_memory {
	uint8_t *pcm_out[OZZY_NUM_PACKETS];
	uint32_t pcm_out_slots;
	uint8_t *pcm_in[OZZY_NUM_PACKETS];
	uint32_t pcm_in_slots;
	uint8_t *isoc_out[ISOC_OUT_TRANSFERS];
	uint32_t isoc_out_size;
	uint8_t *isoc_in;
	uint32_t isoc_in_size;
	uint8_t *midi_in;
	uint8_t *midi_out;
};

/*
 * struct ozzy_usb_endpoint - An endpoint of a claimed interface at its current setting, as its
 * descriptor has it (ozzy_engine_bind_endpoints).
 * @intf:       bInterfaceNumber
 * @max_packet: wMaxPacketSize: bits 0-10 the packet size, bits 11-12 the extra transactions
 *              per microframe (High Speed high bandwidth)
 * @handle:     the platform's own (a descriptor, a pipe), for the endpoints it gets back
 */
struct ozzy_usb_endpoint {
	uint8_t intf;
	uint8_t address;
	uint8_t attributes;
	uint8_t interval;
	uint16_t max_packet;
	void *handle;
};

/* Bytes per service interval: the packet size times the transactions per microframe */
static inline uint16_t ozzy_usb_endpoint_bytes(const struct ozzy_usb_endpoint *ep)
{
	return (uint16_t)((ep->max_packet & 0x07FF) * (((ep->max_packet >> 11) & 3) + 1));
}

/* A pipe the engine runs: bmAttributes & 3, and bytes per service interval */
struct ozzy_engine_pipe {
	uint8_t address;
	uint8_t transfer_type;
	uint8_t interval;
	uint16_t max_packet_size;
};

/*
 * struct ozzy_allocator - The platform's allocator for ozzy_engine_alloc_memory: @alloc
 * returns @size bytes its transfers can use (DMA-capable; buffers never share an
 * OZZY_DMA_ALIGN block), or NULL. It must outlive the memory.
 */
struct ozzy_allocator {
	void *(*alloc)(void *ctx, size_t size);
	void (*free)(void *ctx, void *p);
	void *ctx;
};

struct ozzy_engine;

/*
 * struct ozzy_engine_ops - One per device family. Optional: @isoc_complete, @midi_out_pending,
 * @timer, @setting_quirks, @transfer_frames (default: shm->audio.frames_per_packet). @stop and @suspend end resubmitting (completions
 * with OZZY_STATUS_ABORTED resubmit nothing); the wrappers then abort what is in flight.
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
	uint32_t (*transfer_frames)(const struct ozzy_engine *e, enum ozzy_pcm_dir dir);
};

/*
 * struct ozzy_engine - What every family has, the first member of its own struct.
 * @settings:     what start() sends (ozzy_engine_attach: the defaults)
 * @in_route/@out_route: the route settings' routing, which the ring path applies (on macOS
 *                the HAL routes); updated from ozzy_engine_set_setting() without a lock, as
 *                on Linux: a packet during a change may mix the old and new routing
 * @rate:         the rate start() asks for (ozzy_engine_set_rate; 0: the highest the device
 *                lists)
 * @pcm_ring_set: ozzy_engine_set_pcm_ring(); the completion path picks it up
 * @pcm_ring/@pcm_pos: the rings as the completion path has them, and the ring position of
 *                each one's next frame (ozzy_engine_take_pcm_ring; completion path only)
 * @mem:          where the transfers go (struct ozzy_engine_memory)
 * @pipe:         each role's endpoint as ozzy_engine_set_endpoint() gave it (address 0: none)
 * @allocator:    what @mem came from (ozzy_engine_alloc_memory), NULL: the shared memory
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
	uint32_t rate;
	const struct ozzy_pcm_ring *volatile pcm_ring_set[OZZY_PCM_DIRS];
	const struct ozzy_pcm_ring *pcm_ring[OZZY_PCM_DIRS];
	uint64_t pcm_pos[OZZY_PCM_DIRS];
	struct ozzy_engine_memory mem;
	struct ozzy_engine_pipe pipe[OZZY_PIPE_ROLES];
	const struct ozzy_allocator *allocator;
};

/* Bytes to allocate for @model's engine (0 if Ozzy has none for its family) */
size_t ozzy_engine_size(const struct ozzy_device *model);
/* The engine for @model in @mem (ozzy_engine_size() bytes, any contents), or NULL */
struct ozzy_engine *ozzy_engine_init(void *mem, const struct ozzy_device *model);
/*
 * The bus and shared memory to run on. The settings go back to their defaults and the
 * transfers to the shared memory (freeing any ozzy_engine_alloc_memory gave). The shared memory's
 * header comes from the model: magic, version, USB IDs, and its name as the product name
 * unless the platform put the device's own string there first.
 */
void ozzy_engine_attach(struct ozzy_engine *e, const struct ozzy_bus *bus, volatile struct ozzy_shared_memory *shm);

/*
 * ozzy_engine_alloc_memory - Transfer memory of the platform's own (after ozzy_engine_attach
 * and the endpoints, before ozzy_engine_start): every buffer a separate zeroed allocation from
 * @a, sized for the transfers in flight and the endpoints the engine has, not for the HAL's
 * rings. False if an allocation failed (nothing stays allocated).
 */
bool ozzy_engine_alloc_memory(struct ozzy_engine *e, const struct ozzy_allocator *a);
/* Free what ozzy_engine_alloc_memory gave (the engine is stopped); before freeing the engine */
void ozzy_engine_free_memory(struct ozzy_engine *e);

/* The PCM slot of transfer @idx */
static inline uint8_t *ozzy_engine_pcm_out_slot(const struct ozzy_engine *e, uint32_t idx)
{
	return e->mem.pcm_out[idx % e->mem.pcm_out_slots];
}

static inline uint8_t *ozzy_engine_pcm_in_slot(const struct ozzy_engine *e, uint32_t idx)
{
	return e->mem.pcm_in[idx % e->mem.pcm_in_slots];
}

/*
 * ozzy_engine_bind_endpoints - Pick the pipes the profile asks for from @eps (@count endpoints:
 * every one of the claimed interfaces at their current setting) and tell the engine about
 * them. A role takes its fixed address, or the first endpoint on its interface that
 * ozzy_ep_matches(). Every endpoint is logged, so a log doubles as the descriptor dump of a
 * device nobody captured yet. @bound[role]: the endpoint the platform opens for that role,
 * NULL if the profile doesn't use the role or the device has no such endpoint.
 */
void ozzy_engine_bind_endpoints(struct ozzy_engine *e, const struct ozzy_usb_endpoint *eps, unsigned int count,
				const struct ozzy_usb_endpoint *bound[OZZY_PIPE_ROLES]);

/* One pipe directly (ozzy_engine_bind_endpoints does this for each it picks); tests use it */
static inline void ozzy_engine_set_endpoint(struct ozzy_engine *e, enum ozzy_pipe_role role, uint8_t address,
					    uint8_t transfer_type, uint16_t max_packet_size, uint8_t interval)
{
	struct ozzy_engine_pipe *p = &e->pipe[role];

	p->address = address;
	p->transfer_type = transfer_type;
	p->interval = interval;
	p->max_packet_size = max_packet_size;
	e->ops->set_endpoint(e, role, address, transfer_type, max_packet_size, interval);
}

static inline bool ozzy_engine_start(struct ozzy_engine *e)
{
	return e->ops->start(e);
}

/* No more transfers: stop resubmitting and the timer, abort what is in flight, and tell the
 * HAL the device is gone. Process context. */
static inline void ozzy_engine_stop(struct ozzy_engine *e)
{
	e->ops->stop(e);
	if (e->bus)
		ozzy_bus_abort(e->bus);
}

/*
 * Pause for a restart (ozzy_engine_restart, or the platform's own around a USB reset): like
 * stop, but hardware_present and driver_ready stay set, so the HAL keeps the device published
 * (CoreAudio has stopped IO). ozzy_engine_start() runs the handshake again. Process context.
 */
static inline void ozzy_engine_suspend(struct ozzy_engine *e)
{
	e->ops->suspend(e);
	if (e->bus)
		ozzy_bus_abort(e->bus);
}

/*
 * ozzy_engine_restart - Take the rate and device settings that changed: suspend, then the
 * handshake again. False if the device didn't take it; the engine is stopped then. Process
 * context, not concurrently with itself or stop.
 */
bool ozzy_engine_restart(struct ozzy_engine *e);

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

/*
 * The sample rate the next ozzy_engine_start() asks for, in Hz (0, or one the model doesn't
 * list: the highest it lists). Like a device setting, a running engine takes it with a
 * restart (ozzy_engine_suspend, abort, ozzy_engine_start). Locked to S/PDIF the device runs
 * at the lock rate instead; shm->audio.sample_rate says what it runs at.
 */
static inline void ozzy_engine_set_rate(struct ozzy_engine *e, uint32_t rate)
{
	e->rate = rate;
}

/* What a setting or rate change from the host comes to (ozzy_engine_change_setting/_rate) */
enum ozzy_setting_change {
	OZZY_SETTING_INVALID = -1,   /* the device has no such setting, value or rate (now) */
	OZZY_SETTING_SAME = 0,       /* the value it already has */
	OZZY_SETTING_ROUTED = 1,     /* a route: applied, the next packet has it */
	OZZY_SETTING_RESTART = 2,    /* a device setting or rate: ozzy_engine_restart applies it */
};

/* The rates the host may pick now: ozzy_device_host_rates with the last handshake's clock status */
static inline unsigned int ozzy_engine_host_rates(const struct ozzy_engine *e, uint32_t *out)
{
	uint32_t lock = e->shm && e->shm->audio.settings.clock_locked ? e->shm->audio.settings.lock_rate : 0;

	return ozzy_device_host_rates(e->model, lock, out);
}

/*
 * A rate from the host (ALSA hw_params, the HAL, a WaveRT format): checked against the host
 * rates and logged. OZZY_SETTING_RESTART: ozzy_engine_restart runs the device at it.
 */
enum ozzy_setting_change ozzy_engine_change_rate(struct ozzy_engine *e, uint32_t rate);

/* The rate to ask for: the requested one if the model lists it, else @fallback */
static inline uint32_t ozzy_engine_start_rate(const struct ozzy_engine *e, uint32_t fallback)
{
	return e->rate && ozzy_device_has_rate(e->model, e->rate) ? e->rate : fallback;
}

/*
 * For the families' completion path: the ring attached for @dir, picked up here (a new ring
 * starts at its own start position); NULL if none, or if its channels aren't the model's.
 */
const struct ozzy_pcm_ring *ozzy_engine_take_pcm_ring(struct ozzy_engine *e, enum ozzy_pcm_dir dir);

/*
 * The most frames one PCM transfer of @dir moves, at any rate the device runs at: the
 * smallest period or packet a platform can let its audio stack ask for (after start)
 */
static inline uint32_t ozzy_engine_transfer_frames(const struct ozzy_engine *e, enum ozzy_pcm_dir dir)
{
	if (e->ops->transfer_frames)
		return e->ops->transfer_frames(e, dir);
	return e->shm ? e->shm->audio.frames_per_packet : 0;
}

/* Whether the platform attaches PCM rings (without one attached, PCM out is silence) */
static inline bool ozzy_engine_ring_platform(const struct ozzy_engine *e)
{
	return e->bus->ops->pcm_progress != NULL;
}

/* --- Device settings (common/ozzy_settings.h) --- */

/* The model's quirks as far as settings go (a family drops what its transport can't do) */
uint32_t ozzy_engine_setting_quirks(const struct ozzy_engine *e);
bool ozzy_engine_has_setting(const struct ozzy_engine *e, unsigned int id);
/* How many values setting @id has on this device (0: it doesn't have it) */
unsigned int ozzy_engine_setting_values(const struct ozzy_engine *e, unsigned int id);
/* A device setting takes effect with the next start (ozzy_engine_restart); a route setting at
 * once. No checks: ozzy_engine_change_setting is the one with them. */
void ozzy_engine_set_setting(struct ozzy_engine *e, unsigned int id, uint8_t value);

/* A setting from the user (ALSA control, the HAL, ozzyctl): checked, logged and set */
enum ozzy_setting_change ozzy_engine_change_setting(struct ozzy_engine *e, unsigned int id, unsigned int value);

/* For the families: publish @e->settings and the clock status of the handshake start() just ran */
void ozzy_engine_publish_settings(struct ozzy_engine *e, const struct ozzy_clock_status *clock);

#ifdef __cplusplus
}
#endif

#endif /* OZZY_ENGINE_H */
