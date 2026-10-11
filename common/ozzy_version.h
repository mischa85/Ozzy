/* SPDX-License-Identifier: MIT */
/*
 * Ozzy Build Version
 *
 * OZZY_GIT_VERSION is the commit the driver was built from (`git describe
 * --always --dirty`), passed in by the build: linux/Makefile and
 * macos/install.command. It is logged at startup so bug reports show which
 * code actually ran.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_VERSION_H
#define OZZY_VERSION_H

#ifndef OZZY_GIT_VERSION
#define OZZY_GIT_VERSION "unknown"
#endif

#endif /* OZZY_VERSION_H */
