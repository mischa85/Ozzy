/* SPDX-License-Identifier: MIT */
/*
 * The daemon's mach service (daemon_server.h): one thread receives the clients' calls
 * (struct ozzy_daemon_request) and hands them to the devices; the calls that wait are answered
 * from the device's queue, so a rate change on one device doesn't hold up MIDI on another.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "daemon_server.h"
#include "daemon_device.h"
#include "../../Shared/ozzy_daemon.h"
#include "../../../engine/shared/ozzy_log.h"
#include <IOKit/IOReturn.h>
#include <servers/bootstrap.h>
#include <pthread.h>

static mach_port_t service_port = MACH_PORT_NULL;

void daemon_server_reply(mach_port_t reply, int result)
{
	struct ozzy_daemon_reply msg;
	kern_return_t kr;

	if (reply == MACH_PORT_NULL)
		return;
	memset(&msg, 0, sizeof(msg));
	msg.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
	msg.header.msgh_size = sizeof(msg);
	msg.header.msgh_remote_port = reply;
	msg.header.msgh_id = OZZY_DAEMON_MSG_CALL + 1;
	msg.result = result;
	/* A send-once right never finds its queue full; the timeout only guards a dead client */
	kr = mach_msg(&msg.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(msg), 0, MACH_PORT_NULL, 100,
		      MACH_PORT_NULL);
	if (kr != MACH_MSG_SUCCESS) {
		ozzy_log("reply failed (0x%x)", kr);
		mach_port_deallocate(mach_task_self(), reply);
	}
}

static void handle(struct ozzy_daemon_request *req)
{
	mach_port_t reply = MACH_PORT_NULL;

	/* The reply right comes as a send-once right in the remote port, or not at all */
	if (MACH_MSGH_BITS_REMOTE(req->header.msgh_bits) == MACH_MSG_TYPE_PORT_SEND_ONCE)
		reply = req->header.msgh_remote_port;
	else if (req->header.msgh_remote_port != MACH_PORT_NULL)
		mach_port_deallocate(mach_task_self(), req->header.msgh_remote_port);

	if (req->arg_count > 2) {
		daemon_server_reply(reply, kIOReturnBadArgument);
		return;
	}
	/* Calls that wait need somewhere to answer */
	if (req->method != OZZY_METHOD_MIDI_OUT_PENDING && reply == MACH_PORT_NULL)
		return;
	if (!daemon_device_call(req->location_id, req->session_id, req->method, req->args, req->arg_count, reply))
		daemon_server_reply(reply, kIOReturnNoDevice);
}

static void *server_main(void *arg)
{
	struct {
		struct ozzy_daemon_request req;
		mach_msg_max_trailer_t trailer;
	} buf;
	kern_return_t kr;

	(void)arg;
	pthread_setname_np("ozzy calls");
	for (;;) {
		memset(&buf, 0, sizeof(buf));
		kr = mach_msg(&buf.req.header, MACH_RCV_MSG, 0, sizeof(buf), service_port, MACH_MSG_TIMEOUT_NONE,
			      MACH_PORT_NULL);
		if (kr == MACH_RCV_TOO_LARGE) {
			/* Not one of ours: MACH_RCV_LARGE isn't set, so the kernel dropped it */
			continue;
		}
		if (kr != MACH_MSG_SUCCESS) {
			ozzy_log("receive failed (0x%x), no more calls", kr);
			return NULL;
		}
		if (buf.req.header.msgh_id != OZZY_DAEMON_MSG_CALL || (buf.req.header.msgh_bits & MACH_MSGH_BITS_COMPLEX) ||
		    buf.req.header.msgh_size != sizeof(buf.req)) {
			mach_msg_destroy(&buf.req.header);
			continue;
		}
		handle(&buf.req);
	}
}

bool daemon_server_start(void)
{
	mach_port_limits_t limits;
	pthread_t thread;
	kern_return_t kr = bootstrap_check_in(bootstrap_port, OZZY_DAEMON_SERVICE, &service_port);

	if (kr != KERN_SUCCESS) {
		ozzy_log("no mach service %s (0x%x): the clients can't call the daemon", OZZY_DAEMON_SERVICE, kr);
		return false;
	}
	/* MIDI out nudges come without waiting for room (a full queue drops them): room for many */
	limits.mpl_qlimit = MACH_PORT_QLIMIT_LARGE;
	mach_port_set_attributes(mach_task_self(), service_port, MACH_PORT_LIMITS_INFO, (mach_port_info_t)&limits,
				 MACH_PORT_LIMITS_INFO_COUNT);
	if (pthread_create(&thread, NULL, server_main, NULL) != 0) {
		ozzy_log("can't start the mach service's thread");
		return false;
	}
	pthread_detach(thread);
	return true;
}
