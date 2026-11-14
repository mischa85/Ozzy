/* SPDX-License-Identifier: MIT */
/*
 * A device the daemon drives: what OzzyKext is to one device, in user space. It seizes the
 * USB device from whatever has it open, publishes a shared memory object and a device table
 * entry for the clients (macos/Shared/ozzy_daemon.h), and runs its USB completions on a thread
 * of its own at real-time priority.
 *
 * The devices are kept here: added and removed from the main thread (the IOKit matching
 * notifications), looked up from the mach service's thread.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef DAEMON_DEVICE_H
#define DAEMON_DEVICE_H

#include <IOKit/IOKitLib.h>
#include <stdbool.h>
#include <stdint.h>

struct daemon_device;

/* The device table (OZZY_DAEMON_TABLE_NAME): before the first device, and when the daemon quits */
bool daemon_devices_init(void);
void daemon_devices_exit(void);

/* An IOUSBHostDevice appeared: drive it if it is in the device list and the daemon can have it */
void daemon_device_added(io_service_t service);
/* An IOUSBHostDevice went away: stop driving it if it was one of ours */
void daemon_device_removed(io_service_t service);

/*
 * System sleep and wake, from the main thread (the power notifications): each device suspends
 * before the system sleeps and starts again after it woke, as the kext does (the transfers in
 * flight across a sleep never complete on some devices, the WZ4 in #85). Sleep returns once
 * every device is suspended.
 */
void daemon_devices_sleep(void);
void daemon_devices_wake(void);

/*
 * A call from a client (struct ozzy_daemon_request) for the device at @location_id whose shared
 * memory has @session_id. OZZY_METHOD_MIDI_OUT_PENDING is done at once (no reply); the others
 * are queued on the device, one at a time, and @reply (a send-once right, consumed) gets the
 * IOReturn when done. Returns false if there is no such device (the caller replies).
 */
bool daemon_device_call(uint32_t location_id, uint32_t session_id, uint32_t method, const uint64_t *args,
			uint32_t arg_count, mach_port_t reply);

#endif /* DAEMON_DEVICE_H */
