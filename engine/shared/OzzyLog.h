#pragma once

// Centralized logging macros for consistent prefix usage across the entire Ozzy codebase

#if defined(_KERNEL_MODE)
// ============================================================================
// WINDOWS KERNEL (ACX DRIVER) LOGGING: DebugView / WinDbg, Kd_IHVAUDIO_Mask
// ============================================================================
#include <wdm.h>

#define LogOzzyKext(fmt, ...) DbgPrintEx(DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "[Ozzy] " fmt "\n", ##__VA_ARGS__)
#define LogPloytec(fmt, ...) DbgPrintEx(DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "[Ploytec] " fmt "\n", ##__VA_ARGS__)
#define LogUsbMidi(fmt, ...) DbgPrintEx(DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "[UsbMidi] " fmt "\n", ##__VA_ARGS__)

#elif defined(OZZY_USER_ENGINE)
// ============================================================================
// WINUSB HARNESS LOGGING: stderr
// ============================================================================
#include <stdio.h>

#define LogOzzyKext(fmt, ...) fprintf(stderr, "[Ozzy] " fmt "\n", ##__VA_ARGS__)
#define LogPloytec(fmt, ...) fprintf(stderr, "[Ploytec] " fmt "\n", ##__VA_ARGS__)
#define LogUsbMidi(fmt, ...) fprintf(stderr, "[UsbMidi] " fmt "\n", ##__VA_ARGS__)

#elif defined(KERNEL) || defined(__KERNEL__)
// ============================================================================
// KERNEL (KEXT) LOGGING
// ============================================================================
#include <IOKit/IOLib.h>

// OzzyKext logging (device-agnostic kext code)
#define LogOzzyKext(fmt, ...) IOLog("[OzzyKext] " fmt "\n", ##__VA_ARGS__)

// Ploytec logging (device-specific code in kext)
#define LogPloytec(fmt, ...) IOLog("[Ploytec] " fmt "\n", ##__VA_ARGS__)

// USB MIDI class device logging (Xone:1D)
#define LogUsbMidi(fmt, ...) IOLog("[UsbMidi] " fmt "\n", ##__VA_ARGS__)

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

#endif
