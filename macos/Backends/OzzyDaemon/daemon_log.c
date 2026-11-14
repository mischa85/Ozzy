/* SPDX-License-Identifier: MIT */
/*
 * The daemon's log, and the engine's (engine/shared/ozzy_log.h): the unified log under
 * subsystem OzzyDaemon, which macos/debuglogs.command collects; and stderr as well when it runs
 * in a terminal (by hand, for testing).
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "../../../engine/shared/ozzy_log.h"
#include <os/log.h>
#include <stdio.h>
#include <unistd.h>

void ozzy_daemon_log(const char *fmt, ...)
{
	static os_log_t log;
	static int tty = -1;
	char line[512];
	va_list args;

	if (!log)
		log = os_log_create("OzzyDaemon", "daemon");
	if (tty < 0)
		tty = isatty(STDERR_FILENO);

	va_start(args, fmt);
	vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	os_log(log, "%{public}s", line);
	if (tty)
		fprintf(stderr, "%s\n", line);
}
