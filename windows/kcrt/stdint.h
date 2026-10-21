// SPDX-License-Identifier: MIT
/*
 * stdint.h for the driver: the WDK's kernel CRT has none, and MSVC's own pulls in the user-mode
 * vcruntime.h, which clashes with the kernel CRT (crtdefs.h). Found first through the project's
 * include path (ozzy.vcxproj); the types come from the kernel's basetsd.h.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_KCRT_STDINT_H
#define OZZY_KCRT_STDINT_H

typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long long          int64_t;
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

#ifdef _WIN64
typedef long long          intptr_t;
typedef unsigned long long uintptr_t;
#else
typedef int                intptr_t;
typedef unsigned int       uintptr_t;
#endif

#define INT8_MAX   127
#define INT16_MAX  32767
#define INT32_MAX  2147483647
#define INT64_MAX  9223372036854775807LL
#define UINT8_MAX  0xFFu
#define UINT16_MAX 0xFFFFu
#define UINT32_MAX 0xFFFFFFFFu
#define UINT64_MAX 0xFFFFFFFFFFFFFFFFull

#endif /* OZZY_KCRT_STDINT_H */
