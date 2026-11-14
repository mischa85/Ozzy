/* SPDX-License-Identifier: MIT */
/*
 * The userspace daemon (macos/Backends/OzzyDaemon) and its clients (OzzyHAL, OzzyMIDI): what
 * takes the kext's place when the daemon drives the devices instead.
 *
 * - The device table: a POSIX shared memory object OZZY_DAEMON_TABLE_NAME the daemon writes and
 *   the clients read, one entry per device it drives (in place of matching OzzyKext services).
 * - Per device: a POSIX shared memory object (entry name) holding struct ozzy_shared_memory, as
 *   the kext maps it (in place of IOConnectMapMemory).
 * - The calls: mach messages to the launchd service OZZY_DAEMON_SERVICE (in place of
 *   IOConnectCallScalarMethod), with the kext's selectors (OZZY_METHOD_*) and results
 *   (IOReturn). OZZY_METHOD_MIDI_OUT_PENDING goes without a reply port and gets no reply; the
 *   others block until the daemon has done them, as the kext's do.
 *
 * C, usable from C++: the daemon is the engine side (OZZY_USER_ENGINE), the clients the other
 * one (ozzy_shared_data.h).
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_DAEMON_H
#define OZZY_DAEMON_H

#include <mach/message.h>
#include "../../engine/shared/ozzy_shared_data.h"

#define OZZY_DAEMON_SERVICE       "com.ozzy.daemon"   /* launchd MachServices */
#define OZZY_DAEMON_TABLE_NAME    "/ozzy.devices"
#define OZZY_DAEMON_SHM_FORMAT    "/ozzy.%08x"        /* per device, from its USB locationID */
#define OZZY_DAEMON_TABLE_MAGIC   0x4F5A5A54          /* "OZZT" */
#define OZZY_DAEMON_TABLE_VERSION 1                   /* bump when the table or the messages change */
#define OZZY_DAEMON_MAX_DEVICES   8

/*
 * struct ozzy_daemon_entry - A device the daemon drives. @session_id is its shared memory's, so
 * a client tells a replugged device (a new object under the same name) from the one it mapped.
 * @has_audio: as the kext's OZZY_AUDIO_PROPERTY_KEY (OzzyHAL skips MIDI-only devices); set
 * once the engine started.
 */
struct ozzy_daemon_entry {
	uint32_t used;
	uint32_t location_id;
	uint32_t session_id;
	uint16_t vendor_id;
	uint16_t product_id;
	uint32_t has_audio;
	uint64_t registry_id;               /* the IOUSBHostDevice's registry entry ID */
	char shm_name[32];                  /* POSIX names are at most 31 characters */
};

/*
 * struct ozzy_daemon_table - Written by the daemon's main thread only, under @sequence (a
 * seqlock as struct ozzy_timestamp's: odd while it writes). @magic is 0 once the daemon quit.
 */
struct ozzy_daemon_table {
	uint32_t magic;
	uint32_t version;
	uint32_t daemon_pid;
	ozzy_atomic_u32 sequence;
	struct ozzy_daemon_entry entries[OZZY_DAEMON_MAX_DEVICES];
};

/* The message id of a call; the reply's is one more */
#define OZZY_DAEMON_MSG_CALL      0x4F5A0001

/*
 * struct ozzy_daemon_request - A kext method call for device @location_id, if its shared memory
 * still has @session_id. @args: the scalar inputs, @arg_count of them.
 */
struct ozzy_daemon_request {
	mach_msg_header_t header;
	uint32_t location_id;
	uint32_t session_id;
	uint32_t method;                    /* OZZY_METHOD_* */
	uint32_t arg_count;
	uint64_t args[2];
};

struct ozzy_daemon_reply {
	mach_msg_header_t header;
	int32_t result;                     /* IOReturn */
	uint32_t _pad;
};

#endif /* OZZY_DAEMON_H */
