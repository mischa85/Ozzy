#ifndef OzzyPlatform_h
#define OzzyPlatform_h

// What the engine needs from the OS and the compiler, so the same sources build into the
// macOS kext, the Windows ACX driver and the Windows WinUSB harness.
//
// OZZY_ENGINE_SIDE: this build runs an engine (the kext, the Windows driver, or the harness,
// which defines OZZY_USER_ENGINE). The other side of the shared memory (OzzyHAL, OzzyMIDI)
// uses std::atomic instead.

#include <stdint.h>

#if defined(KERNEL) || defined(_KERNEL_MODE) || defined(OZZY_USER_ENGINE)
#define OZZY_ENGINE_SIDE 1
#endif

// --- Transfer status the bus reports when a transfer was aborted (pipe abort, Stop) ---
#if defined(__APPLE__)
#include <IOKit/IOReturn.h>
#define OZZY_STATUS_ABORTED ((int)kIOReturnAborted)
#elif defined(_KERNEL_MODE)
#include <wdm.h>
#define OZZY_STATUS_ABORTED ((int)STATUS_CANCELLED)
#elif defined(_WIN32)
#define OZZY_STATUS_ABORTED 995   // ERROR_OPERATION_ABORTED, without pulling in windows.h
#endif

// --- errno values the common/ code returns (negated). The Windows kernel has no errno.h;
// the values only travel between Ozzy's own code. ---
#if defined(_KERNEL_MODE)
#define EIO     5
#define EINVAL  22
#elif defined(KERNEL)
#include <sys/errno.h>
#else
#include <errno.h>
#endif

// --- Strings: OzzyVsnprintf (always terminates @dst) and OzzyStrlcpy ---
#include <stddef.h>
#include <stdarg.h>
#if defined(_KERNEL_MODE)
#include <ntstrsafe.h>
static inline void OzzyVsnprintf(char* dst, size_t size, const char* fmt, va_list args) {
    RtlStringCbVPrintfA(dst, size, fmt, args);   // truncates, still terminated
}
#else
#if defined(KERNEL)
#include <libkern/libkern.h>
#else
#include <stdio.h>
#endif
static inline void OzzyVsnprintf(char* dst, size_t size, const char* fmt, va_list args) {
    vsnprintf(dst, size, fmt, args);
}
#endif
#if defined(__APPLE__)
#include <string.h>
#define OzzyStrlcpy strlcpy
#else
static inline void OzzyStrlcpy(char* dst, const char* src, size_t size) {
    size_t i = 0;
    if (!size) return;
    for (; i + 1 < size && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}
#endif

// --- Memory ordering on the engine side ---
// volatile only stops the compiler from reordering; on arm64 the CPU can still make a ring
// index visible before the data it publishes. Pair these with the other side's std::atomic
// acquire loads and release stores.
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#if defined(_M_ARM64)
#define OZZY_HW_FENCE() __dmb(_ARM64_BARRIER_ISH)
#else
// x86/x64 already keep loads and stores in order (TSO); only the compiler must not move them
#define OZZY_HW_FENCE() _ReadWriteBarrier()
#endif
#define OZZY_CACHE_ALIGN __declspec(align(64))

template <typename T> static inline T OzzyLoadAcquire(const volatile T* p) {
    T v = *p;
    OZZY_HW_FENCE();
    return v;
}
template <typename T> static inline void OzzyStoreRelease(volatile T* p, T v) {
    OZZY_HW_FENCE();
    *p = v;
}
template <typename T> static inline void OzzyStoreRelaxed(volatile T* p, T v) { *p = v; }
static inline void OzzyFenceRelease() { OZZY_HW_FENCE(); }
// Set *@p and return what it was, with acquire ordering (a full barrier on MSVC)
static inline bool OzzyExchangeAcquire(bool* p, bool v) {
    return _InterlockedExchange8((volatile char*)p, (char)v) != 0;
}
#else
#define OZZY_CACHE_ALIGN __attribute__((aligned(64)))

template <typename T> static inline T OzzyLoadAcquire(const volatile T* p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
template <typename T> static inline void OzzyStoreRelease(volatile T* p, T v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
template <typename T> static inline void OzzyStoreRelaxed(volatile T* p, T v) {
    __atomic_store_n(p, v, __ATOMIC_RELAXED);
}
static inline void OzzyFenceRelease() { __atomic_thread_fence(__ATOMIC_RELEASE); }
static inline bool OzzyExchangeAcquire(bool* p, bool v) {
    return __atomic_exchange_n(p, v, __ATOMIC_ACQUIRE);
}
#endif

#endif
