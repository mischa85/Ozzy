/* SPDX-License-Identifier: MIT */
/*
 * Endpoint Specs
 *
 * How a device description names one of its endpoints, on every platform: a
 * fixed address, or "the first endpoint on interface N with one of these
 * transfer types, in this direction", the way the official Ploytec driver finds
 * its pipes. ozzy_engine_bind_endpoints() applies it to the endpoints a platform
 * lists.
 *
 * Pure C with no platform dependencies -- usable in both kernel space
 * and userspace on any platform.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_EP_H
#define OZZY_EP_H

#if defined(__KERNEL__) && defined(__linux__)
#include <linux/types.h>
#else
#include <stdint.h>
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Transfer type bits for ozzy_ep.types: 1 << (bmAttributes & 3) */
#define OZZY_EP_ISOC     (1u << 1)
#define OZZY_EP_BULK     (1u << 2)
#define OZZY_EP_INT      (1u << 3)

/* Directions for ozzy_ep.dir (the bEndpointAddress direction bit) */
#define OZZY_EP_DIR_OUT  0x00
#define OZZY_EP_DIR_IN   0x80

/*
 * struct ozzy_ep - One endpoint of a device description.
 * @intf:    interface number (searched when @address is 0)
 * @address: bEndpointAddress including the direction bit, or 0 to search
 * @types:   OZZY_EP_xxx bits accepted when searching
 * @dir:     OZZY_EP_DIR_xxx when searching
 * All zero: the device has no such endpoint.
 */
struct ozzy_ep {
	uint8_t intf;
	uint8_t address;
	uint8_t types;
	uint8_t dir;
};

/*
 * ozzy_ep_matches - Whether an endpoint with this bEndpointAddress and
 * bmAttributes is one that @spec searches for.
 */
static inline bool ozzy_ep_matches(const struct ozzy_ep *spec, uint8_t address, uint8_t attributes)
{
	return (address & 0x80) == spec->dir && (spec->types & (1u << (attributes & 3)));
}

#ifdef __cplusplus
}
#endif

#endif /* OZZY_EP_H */
