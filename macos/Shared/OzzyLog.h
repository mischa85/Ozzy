#pragma once

// Logging for the macOS user-space plugins (OzzyHAL, OzzyMIDI); the engine logs through
// engine/shared/ozzy_log.h

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
