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

#ifndef OZZY_ENGINE_SIDE
/*
 * --- The client side (OzzyHAL, OzzyMIDI) ---
 * Each keeps the table mapped while it is valid, reads the entries with
 * ozzy_daemon_table_read, maps the devices it takes with ozzy_daemon_map and calls with
 * ozzy_daemon_call. A daemon that crashed leaves its objects behind with their magic: its pid
 * tells (ozzy_daemon_alive).
 */
#include <mach/mach.h>
#include <servers/bootstrap.h>
#include <IOKit/IOReturn.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>

/* Whether process @pid runs (it is root's: EPERM means it does) */
static inline bool ozzy_daemon_alive(uint32_t pid)
{
	return pid && (kill((pid_t)pid, 0) == 0 || errno == EPERM);
}

/* The table of a running daemon, mapped read-only; NULL if there is none */
static inline const struct ozzy_daemon_table *ozzy_daemon_table_open(void)
{
	int fd = shm_open(OZZY_DAEMON_TABLE_NAME, O_RDONLY);
	void *p;

	if (fd < 0)
		return NULL;
	p = mmap(NULL, sizeof(struct ozzy_daemon_table), PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	return p == MAP_FAILED ? NULL : (const struct ozzy_daemon_table *)p;
}

static inline void ozzy_daemon_table_close(const struct ozzy_daemon_table *t)
{
	if (t)
		munmap((void *)t, sizeof(*t));
}

/* Whether @t still belongs to a running daemon of this version (else close it, open it again) */
static inline bool ozzy_daemon_table_valid(const struct ozzy_daemon_table *t)
{
	return t && t->magic == OZZY_DAEMON_TABLE_MAGIC && t->version == OZZY_DAEMON_TABLE_VERSION &&
	       ozzy_daemon_alive(t->daemon_pid);
}

/* A consistent copy of the entries (OZZY_DAEMON_MAX_DEVICES of them); false if the daemon kept
 * the table busy (try again next time) */
static inline bool ozzy_daemon_table_read(const struct ozzy_daemon_table *t, struct ozzy_daemon_entry *entries)
{
	for (unsigned int tries = 0; tries < 100; tries++) {
		uint32_t seq = t->sequence.load(std::memory_order_acquire);

		if (seq & 1)
			continue;
		memcpy(entries, (const void *)t->entries, sizeof(t->entries));
		std::atomic_thread_fence(std::memory_order_acquire);
		if (t->sequence.load(std::memory_order_relaxed) == seq)
			return true;
	}
	return false;
}

/* Device @e's shared memory, read-write; NULL if it is gone or no longer @e's session */
static inline struct ozzy_shared_memory *ozzy_daemon_map(const struct ozzy_daemon_entry *e)
{
	char name[sizeof(e->shm_name) + 1];
	struct ozzy_shared_memory *shm;
	int fd;
	void *p;

	memcpy(name, e->shm_name, sizeof(e->shm_name));
	name[sizeof(e->shm_name)] = 0;
	fd = shm_open(name, O_RDWR);
	if (fd < 0)
		return NULL;
	p = mmap(NULL, sizeof(struct ozzy_shared_memory), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (p == MAP_FAILED)
		return NULL;
	shm = (struct ozzy_shared_memory *)p;
	if (shm->session_id != e->session_id) {
		munmap(p, sizeof(*shm));
		return NULL;
	}
	return shm;
}

static inline void ozzy_daemon_unmap(struct ozzy_shared_memory *shm)
{
	if (shm)
		munmap((void *)shm, sizeof(*shm));
}

/* A send right to the daemon's service, looked up once (again after @renew: the daemon
 * restarted); MACH_PORT_NULL if there is none */
static inline mach_port_t ozzy_daemon_service(bool renew)
{
	static std::atomic<mach_port_t> service { MACH_PORT_NULL };
	mach_port_t p = service.load(std::memory_order_acquire);
	mach_port_t found = MACH_PORT_NULL;

	if (renew && p != MACH_PORT_NULL) {
		if (service.compare_exchange_strong(p, MACH_PORT_NULL))
			mach_port_deallocate(mach_task_self(), p);
		p = service.load(std::memory_order_acquire);
	}
	if (p != MACH_PORT_NULL)
		return p;
	if (bootstrap_look_up(bootstrap_port, OZZY_DAEMON_SERVICE, &found) != KERN_SUCCESS)
		return MACH_PORT_NULL;
	if (!service.compare_exchange_strong(p, found)) {
		mach_port_deallocate(mach_task_self(), found);
		return p;
	}
	return found;
}

/*
 * The kext's external method @method with @args (@count, at most 2) for the device at
 * @location_id with @session_id. @wait: until the daemon has done it (its IOReturn); else sent
 * and forgotten (OZZY_METHOD_MIDI_OUT_PENDING), never blocking. Returns an IOReturn either way.
 */
static inline kern_return_t ozzy_daemon_call(uint32_t location_id, uint32_t session_id, uint32_t method,
					     const uint64_t *args, uint32_t count, bool wait)
{
	for (int attempt = 0; attempt < 2; attempt++) {
		mach_port_t service = ozzy_daemon_service(attempt > 0);
		mach_port_t reply = MACH_PORT_NULL;
		union {
			struct ozzy_daemon_request req;
			struct {
				struct ozzy_daemon_reply rep;
				mach_msg_max_trailer_t trailer;
			} in;
		} msg;
		kern_return_t kr;

		if (service == MACH_PORT_NULL)
			return kIOReturnNotAttached;
		if (wait && mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &reply) != KERN_SUCCESS)
			return kIOReturnNoResources;

		memset(&msg, 0, sizeof(msg));
		msg.req.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, wait ? MACH_MSG_TYPE_MAKE_SEND_ONCE : 0);
		msg.req.header.msgh_size = sizeof(msg.req);
		msg.req.header.msgh_remote_port = service;
		msg.req.header.msgh_local_port = reply;
		msg.req.header.msgh_id = OZZY_DAEMON_MSG_CALL;
		msg.req.location_id = location_id;
		msg.req.session_id = session_id;
		msg.req.method = method;
		msg.req.arg_count = count > 2 ? 2 : count;
		for (uint32_t i = 0; i < msg.req.arg_count; i++)
			msg.req.args[i] = args[i];

		if (wait)
			/* A restart takes about a second; a device that hangs, the requests' 5 s each */
			kr = mach_msg(&msg.req.header, MACH_SEND_MSG | MACH_RCV_MSG | MACH_RCV_TIMEOUT, sizeof(msg.req),
				      sizeof(msg.in), reply, 30000, MACH_PORT_NULL);
		else
			kr = mach_msg(&msg.req.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(msg.req), 0,
				      MACH_PORT_NULL, 0, MACH_PORT_NULL);
		if (reply != MACH_PORT_NULL)
			mach_port_mod_refs(mach_task_self(), reply, MACH_PORT_RIGHT_RECEIVE, -1);

		if (kr == MACH_SEND_INVALID_DEST)
			continue;   /* the daemon restarted: look it up again */
		if (kr != MACH_MSG_SUCCESS)
			return kr;
		if (!wait)
			return kIOReturnSuccess;
		if (msg.in.rep.header.msgh_id != OZZY_DAEMON_MSG_CALL + 1)
			return kIOReturnError;
		return msg.in.rep.result;
	}
	return kIOReturnNotAttached;
}
#endif /* !OZZY_ENGINE_SIDE */

#endif /* OZZY_DAEMON_H */
