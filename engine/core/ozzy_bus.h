/* SPDX-License-Identifier: MIT */
/*
 * What an engine needs from the platform's USB stack: transfers on the shared memory, control
 * requests, a timer and a clock. The macOS kext (KextBus), the Windows driver (kmdf_bus.c)
 * and the WinUSB harness (winusb_bus.c) each fill in a struct ozzy_bus_ops. Completions come back
 * through ozzy_engine_complete() and ozzy_engine_isoc_complete().
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_BUS_H
#define OZZY_BUS_H

#include "../shared/ozzy_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OZZY_MAX_ISOC_PACKETS 32   /* packets per isochronous transfer (4 ms of microframes) */

/*
 * Transfers that can be in flight at the same time never share an OZZY_DMA_ALIGN block of
 * memory: the largest cache line DMA has to respect on a non-coherent machine (arm64 Linux's
 * ARCH_DMA_MINALIGN), so one transfer's cache maintenance can't clobber another's data.
 */
#define OZZY_DMA_ALIGN 128
#define OZZY_DMA_ROUND(n) (((n) + OZZY_DMA_ALIGN - 1) & ~(uint32_t)(OZZY_DMA_ALIGN - 1))

/*
 * One packet (service interval) of an isochronous transfer. The engine fills @offset and
 * @length; the bus fills @actual, @status and @time (the platform's clock) on completion.
 * @offset may run past the end of the buffer and wrap to its start.
 */
struct ozzy_isoc_packet {
	uint32_t offset;
	uint32_t length;
	uint32_t actual;
	int32_t status;
	uint64_t time;
};

enum ozzy_pcm_dir {
	OZZY_PCM_OUT,
	OZZY_PCM_IN,
	OZZY_PCM_DIRS,
};

/*
 * struct ozzy_bus_ops - The platform's services. @ctx is struct ozzy_bus.ctx.
 * @submit:       queue a bulk or interrupt transfer of @size bytes at @buf (in the shared
 *                memory) on endpoint @pipe; @xfer comes back with the completion
 * @submit_isoc:  queue an isochronous transfer of @count (at most OZZY_MAX_ISOC_PACKETS)
 *                @packets over @buf (@buf_size bytes), starting at the next frame; @packets
 *                stay valid until the completion. Optional.
 * @control:      a control request, synchronous (the handshake)
 * @clear_halt:   clear ENDPOINT_HALT on endpoint @endpoint (with direction bit) and reset the
 *                host side of its pipe, synchronous (the handshake); 0 or a negative error.
 *                Optional.
 * @start_timer:  call ozzy_engine_timer() every @interval_us until @stop_timer, which waits
 *                for a call in progress. Optional.
 * @pcm_progress: the engine moved PCM through an attached ring (ozzy_engine_set_pcm_ring):
 *                @frames is the ring position after the packet. From the completion path.
 *                Optional: a platform that attaches rings has it, and without a ring
 *                attached its PCM out is silence (macOS has none: the HAL fills the
 *                shared memory itself).
 * @sleep_ms:     sleep (handshake only, never from a completion)
 * @time:         the platform's clock (macOS mach_absolute_time, Windows QPC)
 * @abort:        cancel every transfer in flight and return once their completions have run
 *                (with OZZY_STATUS_ABORTED); the pipes stay usable. Process context, never
 *                from a completion. The engine calls it when it stops or suspends.
 */
struct ozzy_bus_ops {
	bool (*submit)(void *ctx, uint8_t pipe, void *buf, uint32_t size, void *xfer);
	bool (*submit_isoc)(void *ctx, uint8_t pipe, void *buf, uint32_t buf_size,
			    struct ozzy_isoc_packet *packets, uint32_t count, void *xfer);
	bool (*control)(void *ctx, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
			void *data, uint16_t length);
	int (*clear_halt)(void *ctx, uint8_t endpoint);
	bool (*start_timer)(void *ctx, uint32_t interval_us);
	void (*stop_timer)(void *ctx);
	void (*pcm_progress)(void *ctx, enum ozzy_pcm_dir dir, uint64_t frames);
	void (*sleep_ms)(void *ctx, unsigned int ms);
	uint64_t (*time)(void *ctx);
	void (*abort)(void *ctx);
};

struct ozzy_bus {
	const struct ozzy_bus_ops *ops;
	void *ctx;
};

static inline bool ozzy_bus_submit(const struct ozzy_bus *bus, uint8_t pipe, void *buf, uint32_t size,
				   void *xfer)
{
	return bus->ops->submit(bus->ctx, pipe, buf, size, xfer);
}

static inline bool ozzy_bus_submit_isoc(const struct ozzy_bus *bus, uint8_t pipe, void *buf, uint32_t buf_size,
					struct ozzy_isoc_packet *packets, uint32_t count, void *xfer)
{
	return bus->ops->submit_isoc && bus->ops->submit_isoc(bus->ctx, pipe, buf, buf_size, packets, count, xfer);
}

static inline bool ozzy_bus_start_timer(const struct ozzy_bus *bus, uint32_t interval_us)
{
	return bus->ops->start_timer && bus->ops->start_timer(bus->ctx, interval_us);
}

static inline void ozzy_bus_stop_timer(const struct ozzy_bus *bus)
{
	if (bus->ops->stop_timer)
		bus->ops->stop_timer(bus->ctx);
}

static inline void ozzy_bus_pcm_progress(const struct ozzy_bus *bus, enum ozzy_pcm_dir dir, uint64_t frames)
{
	if (bus->ops->pcm_progress)
		bus->ops->pcm_progress(bus->ctx, dir, frames);
}

static inline void ozzy_bus_abort(const struct ozzy_bus *bus)
{
	bus->ops->abort(bus->ctx);
}

static inline uint64_t ozzy_bus_time(const struct ozzy_bus *bus)
{
	return bus->ops->time(bus->ctx);
}

#ifdef __cplusplus
}
#endif

#endif /* OZZY_BUS_H */
