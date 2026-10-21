/* SPDX-License-Identifier: MIT */
/*
 * Ozzy Build Version
 *
 * OZZY_GIT_VERSION is the commit the driver was built from, passed in by the
 * build (linux/Makefile, macos/install.command, windows/scripts/build.ps1 and
 * windows/driver/ozzy.vcxproj): `git describe --always --dirty` in Ozzy's own
 * checkout (not a repository the sources were unpacked into), else
 * common/ozzy_version.txt, where git archive and GitHub's ZIP downloads write
 * the commit (export-subst, .gitattributes), else "unknown". It is logged at
 * startup so bug reports show which code actually ran.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_VERSION_H
#define OZZY_VERSION_H

#ifndef OZZY_GIT_VERSION
#define OZZY_GIT_VERSION "unknown"
#endif

#endif /* OZZY_VERSION_H */
