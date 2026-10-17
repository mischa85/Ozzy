/* SPDX-License-Identifier: MIT */
/*
 * What the engine needs from the OS and the compiler, so the same sources build into the
 * macOS kext, the Windows ACX driver and the Windows WinUSB harness. C, usable from C++.
 *
 * OZZY_ENGINE_SIDE: this build runs an engine (the kext, the Windows driver, or the harness,
 * which defines OZZY_USER_ENGINE). The other side of the shared memory (OzzyHAL, OzzyMIDI)
 * uses std::atomic instead.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_PLATFORM_H
#define OZZY_PLATFORM_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#if defined(KERNEL) || defined(_KERNEL_MODE) || defined(OZZY_USER_ENGINE)
#define OZZY_ENGINE_SIDE 1
#endif

/* --- Transfer status the bus reports when a transfer was aborted (pipe abort, stop) --- */
#if defined(__APPLE__)
#include <IOKit/IOReturn.h>
#define OZZY_STATUS_ABORTED ((int)kIOReturnAborted)
#elif defined(_KERNEL_MODE)
#include <wdm.h>
#define OZZY_STATUS_ABORTED ((int)STATUS_CANCELLED)
#elif defined(_WIN32)
#define OZZY_STATUS_ABORTED 995   /* ERROR_OPERATION_ABORTED, without pulling in windows.h */
#endif

/*
 * --- errno values the common/ code returns (negated) ---
 * The Windows kernel has no errno.h; the values only travel between Ozzy's own code.
 */
#if defined(_KERNEL_MODE)
#define EIO     5
#define EINVAL  22
#define ENODEV  19
#elif defined(KERNEL)
#include <sys/errno.h>
#else
#include <errno.h>
#endif

/* --- Strings: ozzy_vsnprintf (always terminates @dst) and ozzy_strlcpy --- */
#if defined(_KERNEL_MODE)
#include <ntstrsafe.h>
static inline void ozzy_vsnprintf(char *dst, size_t size, const char *fmt, va_list args)
{
	RtlStringCbVPrintfA(dst, size, fmt, args);   /* truncates, still terminated */
}
#else
#if defined(KERNEL)
#include <libkern/libkern.h>
#else
#include <stdio.h>
#endif
static inline void ozzy_vsnprintf(char *dst, size_t size, const char *fmt, va_list args)
{
	vsnprintf(dst, size, fmt, args);
}
#endif

static inline void ozzy_strlcpy(char *dst, const char *src, size_t size)
{
	size_t i = 0;

	if (!size)
		return;
	for (; i + 1 < size && src[i]; i++)
		dst[i] = src[i];
	dst[i] = 0;
}

/*
 * --- Memory ordering on the engine side ---
 * volatile only stops the compiler from reordering; on arm64 the CPU can still make a ring
 * index visible before the data it publishes. Pair these with the other side's std::atomic
 * acquire loads and release stores.
 */
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#if defined(_M_ARM64)
#define OZZY_HW_FENCE() __dmb(_ARM64_BARRIER_ISH)
#else
/* x86/x64 already keep loads and stores in order (TSO); only the compiler must not move them */
#define OZZY_HW_FENCE() _ReadWriteBarrier()
#endif
#define OZZY_CACHE_ALIGN __declspec(align(64))

#define OZZY_LOAD_ACQUIRE(type, p)       { type v = *(p); OZZY_HW_FENCE(); return v; }
#define OZZY_STORE_RELEASE(p, v)         { OZZY_HW_FENCE(); *(p) = (v); }
#define OZZY_STORE_RELAXED(p, v)         { *(p) = (v); }

static inline void ozzy_fence_release(void) { OZZY_HW_FENCE(); }

/* Set *@p to @v and return what it was, with acquire ordering (a full barrier on MSVC) */
static inline bool ozzy_xchg_acquire_bool(volatile bool *p, bool v)
{
	return _InterlockedExchange8((volatile char *)p, (char)v) != 0;
}
#else
#define OZZY_CACHE_ALIGN __attribute__((aligned(64)))

#define OZZY_LOAD_ACQUIRE(type, p)       { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
#define OZZY_STORE_RELEASE(p, v)         { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
#define OZZY_STORE_RELAXED(p, v)         { __atomic_store_n(p, v, __ATOMIC_RELAXED); }

static inline void ozzy_fence_release(void) { __atomic_thread_fence(__ATOMIC_RELEASE); }

static inline bool ozzy_xchg_acquire_bool(volatile bool *p, bool v)
{
	return __atomic_exchange_n(p, v, __ATOMIC_ACQUIRE);
}
#endif

static inline uint32_t ozzy_load_acquire_u32(const volatile uint32_t *p) OZZY_LOAD_ACQUIRE(uint32_t, p)
static inline uint64_t ozzy_load_acquire_u64(const volatile uint64_t *p) OZZY_LOAD_ACQUIRE(uint64_t, p)
static inline const void *ozzy_load_acquire_ptr(const void *const volatile *p) OZZY_LOAD_ACQUIRE(const void *, p)
static inline void ozzy_store_release_u32(volatile uint32_t *p, uint32_t v) OZZY_STORE_RELEASE(p, v)
static inline void ozzy_store_release_u64(volatile uint64_t *p, uint64_t v) OZZY_STORE_RELEASE(p, v)
static inline void ozzy_store_release_bool(volatile bool *p, bool v) OZZY_STORE_RELEASE(p, v)
static inline void ozzy_store_release_ptr(const void *volatile *p, const void *v) OZZY_STORE_RELEASE(p, v)
static inline void ozzy_store_relaxed_u32(volatile uint32_t *p, uint32_t v) OZZY_STORE_RELAXED(p, v)
static inline void ozzy_store_relaxed_u64(volatile uint64_t *p, uint64_t v) OZZY_STORE_RELAXED(p, v)

#endif /* OZZY_PLATFORM_H */
