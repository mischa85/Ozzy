#pragma once

// Centralized logging macros for consistent prefix usage across the entire Ozzy codebase

#if defined(KERNEL) || defined(__KERNEL__)
// ============================================================================
// KERNEL (KEXT) LOGGING
// ============================================================================
#include <IOKit/IOLib.h>

// OzzyKext logging (device-agnostic kext code)
#define LogOzzyKext(fmt, ...) IOLog("[OzzyKext] " fmt "\n", ##__VA_ARGS__)

// Ploytec logging (device-specific code in kext)
#define LogPloytec(fmt, ...) IOLog("[Ploytec] " fmt "\n", ##__VA_ARGS__)

#else
// ============================================================================
// USERSPACE LOGGING (HAL, MIDI, Daemon)
// ============================================================================
#include <os/log.h>

// Helper to get subsystem logs
static inline os_log_t GetOzzyHALLog() {
    static os_log_t log = NULL;
    if (!log) log = os_log_create("OzzyHAL", "plugin");
    return log;
}

static inline os_log_t GetOzzyMIDILog() {
    static os_log_t log = NULL;
    if (!log) log = os_log_create("OzzyMIDI", "driver");
    return log;
}

// Logging macros for HAL
#define LogOzzyHAL(fmt, ...) os_log(GetOzzyHALLog(), "[OzzyHAL] " fmt, ##__VA_ARGS__)
#define LogOzzyHALError(fmt, ...) os_log_error(GetOzzyHALLog(), "[OzzyHAL] " fmt, ##__VA_ARGS__)

// Logging macros for MIDI
#define LogOzzyMIDI(fmt, ...) os_log_info(GetOzzyMIDILog(), "[OzzyMIDI] " fmt, ##__VA_ARGS__)
#define LogOzzyMIDIError(fmt, ...) os_log_error(GetOzzyMIDILog(), "[OzzyMIDI] " fmt, ##__VA_ARGS__)

/* Kext macros available in userspace (clangd, unit tests). */
#define LogOzzyKext(fmt, ...) os_log(OS_LOG_DEFAULT, "[OzzyKext] " fmt, ##__VA_ARGS__)
#define LogPloytec(fmt, ...)  os_log(OS_LOG_DEFAULT, "[Ploytec] "  fmt, ##__VA_ARGS__)

#endif
