/* SPDX-License-Identifier: MIT */
/*
 * The daemon's mach service (OZZY_DAEMON_SERVICE): the clients' calls, on a thread of its own.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef DAEMON_SERVER_H
#define DAEMON_SERVER_H

#include <mach/mach.h>
#include <stdbool.h>

/* Check in with launchd and start answering; false if that fails. Run by hand as root, the
 * check-in registers the name all the same (macOS 26), so the clients can call it then too. */
bool daemon_server_start(void);

/* Answer a call: @result (an IOReturn) to @reply, a send-once right (consumed) */
void daemon_server_reply(mach_port_t reply, int result);

#endif /* DAEMON_SERVER_H */
