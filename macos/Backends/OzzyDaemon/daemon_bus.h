/* SPDX-License-Identifier: MIT */
/*
 * The engine's bus (engine/core/ozzy_bus.h) on IOUSBLib, for the daemon: what KextBus is to the
 * kext. Transfers go straight to and from the shared memory, as the kext's do (an isochronous
 * transfer whose packets wrap round the ring goes through a buffer of its own: IOUSBLib takes
 * one contiguous buffer per transfer). Every completion runs on the device's USB thread (its
 * run loop has the interfaces' async event sources), so the engine's callbacks never run
 * concurrently with each other.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef DAEMON_BUS_H
#define DAEMON_BUS_H

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/usb/IOUSBLib.h>
#include <pthread.h>
#include "../../../engine/core/ozzy_engine.h"

#define DAEMON_BUS_INTERFACES 4
#define DAEMON_BUS_PIPE_SLOTS 32    /* endpoint number in bits 0-3, direction in bit 4 (as KextBus) */

typedef IOUSBDeviceInterface650 **daemon_usb_device_t;
typedef IOUSBInterfaceInterface650 **daemon_usb_interface_t;

struct daemon_bus_pipe {
	daemon_usb_interface_t intf;    /* NULL: no such pipe */
	uint8_t ref;                    /* IOUSBLib's pipeRef on @intf */
	uint8_t interval;               /* bInterval */
};

/* Zero it, then daemon_bus_init */
struct daemon_bus {
	struct ozzy_bus bus;            /* what the engine runs on (ozzy_engine_attach) */
	struct ozzy_engine *engine;
	daemon_usb_device_t dev;
	bool high_speed;
	CFRunLoopRef run_loop;          /* the USB thread's */
	pthread_t thread;               /* the USB thread */

	daemon_usb_interface_t intf[DAEMON_BUS_INTERFACES];   /* the profile's, in its order */
	CFRunLoopSourceRef source[DAEMON_BUS_INTERFACES];
	int intf_count;
	struct daemon_bus_pipe pipe[DAEMON_BUS_PIPE_SLOTS];
	uint64_t isoc_next_frame[DAEMON_BUS_PIPE_SLOTS];
	volatile int32_t pending;       /* transfers submitted and not completed */

	/* For the stall watchdog (daemon_device): when each pipe last completed, and whether the
	 * engine submitted on it since daemon_bus_watch_reset */
	volatile uint64_t last_done[DAEMON_BUS_PIPE_SLOTS];
	volatile bool submitted[DAEMON_BUS_PIPE_SLOTS];

	/* The engine's timer, on the USB thread's run loop */
	CFRunLoopTimerRef timer;
	volatile bool timer_on;
	volatile bool timer_busy;
};

/* @dev is open; @run_loop and @thread are the USB thread's */
void daemon_bus_init(struct daemon_bus *b, daemon_usb_device_t dev, CFRunLoopRef run_loop, pthread_t thread,
		     struct ozzy_engine *engine);

/* What OzzyKext::ConfigureHardware does: configuration 1, the profile's interfaces at their
 * alternate settings, every endpoint to ozzy_engine_bind_endpoints and the pipes opened */
bool daemon_bus_configure(struct daemon_bus *b);

/* Give the interfaces back (the engine is stopped: nothing in flight) */
void daemon_bus_close(struct daemon_bus *b);

/* The engine's USB speed (enum ozzy_usb_speed) */
enum ozzy_usb_speed daemon_bus_speed(struct daemon_bus *b);

static inline int daemon_bus_slot(uint8_t address)
{
	return (address & 0x0F) | ((address & 0x80) >> 3);
}

/* Every pipe as if it just completed, none submitted: after a start, which takes longer than
 * the watchdog's limit */
void daemon_bus_watch_reset(struct daemon_bus *b);

#endif /* DAEMON_BUS_H */
