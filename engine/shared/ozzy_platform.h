/* SPDX-License-Identifier: MIT */
/*
 * What the engine needs from the OS and the compiler, so the same sources build into the
 * Linux module, the macOS kext, the Windows ACX driver, the Windows WinUSB harness and the
 * host tests. C, usable from C++.
 *
 * OZZY_ENGINE_SIDE: this build runs an engine (a kernel driver, or the harness and the tests,
 * which define OZZY_USER_ENGINE). The other side of the shared memory (OzzyHAL, OzzyMIDI)
 * uses std::atomic instead.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_PLATFORM_H
#define OZZY_PLATFORM_H

#if defined(__KERNEL__) && defined(__linux__)
#define OZZY_LINUX_KERNEL 1
#endif

#ifdef OZZY_LINUX_KERNEL
#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/stdarg.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <asm/barrier.h>
#include <linux/atomic.h>
#else
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif
#endif

#if defined(KERNEL) || defined(_KERNEL_MODE) || defined(OZZY_LINUX_KERNEL) || defined(OZZY_USER_ENGINE)
#define OZZY_ENGINE_SIDE 1
#endif

/*
 * --- Transfer status the bus reports when a transfer was aborted (pipe abort, stop) ---
 * The Linux bus maps a killed or unlinked URB (-ENOENT, -ECONNRESET, -ESHUTDOWN) to this.
 */
#if defined(OZZY_LINUX_KERNEL)
#define OZZY_STATUS_ABORTED (-ENOENT)
#elif defined(__APPLE__)
#include <IOKit/IOReturn.h>
#define OZZY_STATUS_ABORTED ((int)kIOReturnAborted)
#elif defined(_KERNEL_MODE)
#include <wdm.h>
#define OZZY_STATUS_ABORTED ((int)STATUS_CANCELLED)
#elif defined(_WIN32)
#define OZZY_STATUS_ABORTED 995   /* ERROR_OPERATION_ABORTED, without pulling in windows.h */
#else
#define OZZY_STATUS_ABORTED (-125)  /* -ECANCELED: host builds (tests); a bus maps its own */
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
#elif !defined(OZZY_LINUX_KERNEL)
#include <errno.h>
#endif

/* printf-style format checking where the compiler has it */
#if defined(__GNUC__) || defined(__clang__)
#define OZZY_PRINTF_FORMAT(fmt, args) __attribute__((format(printf, fmt, args)))
#else
#define OZZY_PRINTF_FORMAT(fmt, args)
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
#elif !defined(OZZY_LINUX_KERNEL)
#include <stdio.h>
#endif
static inline OZZY_PRINTF_FORMAT(3, 0) void ozzy_vsnprintf(char *dst, size_t size, const char *fmt, va_list args)
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

/* --- @a % @b for a 64-bit position (32-bit Linux has no 64-bit division in the kernel) --- */
static inline uint32_t ozzy_mod_u64(uint64_t a, uint32_t b)
{
#ifdef OZZY_LINUX_KERNEL
	uint32_t rem;

	div_u64_rem(a, b, &rem);
	return rem;
#else
	return (uint32_t)(a % b);
#endif
}

/*
 * --- Memory ordering on the engine side ---
 * volatile only stops the compiler from reordering; on arm64 the CPU can still make a ring
 * index visible before the data it publishes. Pair these with the other side's acquire loads
 * and release stores.
 */
#if defined(OZZY_LINUX_KERNEL)
/* The kernel's own primitives: __atomic on 64-bit values would need libatomic on 32-bit ARM.
 * 64-bit values are only written relaxed (the timestamp seqlock covers tearing). */
#define OZZY_CACHE_ALIGN __attribute__((aligned(64)))

static inline uint32_t ozzy_load_acquire_u32(const volatile uint32_t *p) { return smp_load_acquire(p); }
static inline const void *ozzy_load_acquire_ptr(const void *const volatile *p) { return smp_load_acquire(p); }
static inline void ozzy_store_release_u32(volatile uint32_t *p, uint32_t v) { smp_store_release(p, v); }
static inline void ozzy_store_release_bool(volatile bool *p, bool v) { smp_store_release(p, v); }
static inline void ozzy_store_release_ptr(const void *volatile *p, const void *v) { smp_store_release(p, v); }
static inline void ozzy_store_relaxed_u32(volatile uint32_t *p, uint32_t v) { WRITE_ONCE(*p, v); }
static inline void ozzy_store_relaxed_u64(volatile uint64_t *p, uint64_t v) { WRITE_ONCE(*p, v); }
static inline void ozzy_fence_release(void) { smp_wmb(); }
static inline void ozzy_fence_acquire(void) { smp_rmb(); }
static inline uint64_t ozzy_load_relaxed_u64(const volatile uint64_t *p) { return READ_ONCE(*p); }
static inline bool ozzy_xchg_acquire_bool(volatile bool *p, bool v) { return xchg_acquire(p, v); }
#else
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
static inline void ozzy_fence_acquire(void) { OZZY_HW_FENCE(); }
static inline uint64_t ozzy_load_relaxed_u64(const volatile uint64_t *p) { return *p; }

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
static inline void ozzy_fence_acquire(void) { __atomic_thread_fence(__ATOMIC_ACQUIRE); }
static inline uint64_t ozzy_load_relaxed_u64(const volatile uint64_t *p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }

static inline bool ozzy_xchg_acquire_bool(volatile bool *p, bool v)
{
	return __atomic_exchange_n(p, v, __ATOMIC_ACQUIRE);
}
#endif

static inline uint32_t ozzy_load_acquire_u32(const volatile uint32_t *p) OZZY_LOAD_ACQUIRE(uint32_t, p)
static inline const void *ozzy_load_acquire_ptr(const void *const volatile *p) OZZY_LOAD_ACQUIRE(const void *, p)
static inline void ozzy_store_release_u32(volatile uint32_t *p, uint32_t v) OZZY_STORE_RELEASE(p, v)
static inline void ozzy_store_release_bool(volatile bool *p, bool v) OZZY_STORE_RELEASE(p, v)
static inline void ozzy_store_release_ptr(const void *volatile *p, const void *v) OZZY_STORE_RELEASE(p, v)
static inline void ozzy_store_relaxed_u32(volatile uint32_t *p, uint32_t v) OZZY_STORE_RELAXED(p, v)
static inline void ozzy_store_relaxed_u64(volatile uint64_t *p, uint64_t v) OZZY_STORE_RELAXED(p, v)
#endif

#endif /* OZZY_PLATFORM_H */
