/* SPDX-License-Identifier: MIT */
/*
 * Engine logging: one line per call, with the component as prefix, wherever the engine runs.
 * (The macOS HAL and MIDI plugins log through macos/Shared/OzzyLog.h.)
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_LOG_H
#define OZZY_LOG_H

#if defined(_KERNEL_MODE)
/* Windows kernel (ACX driver): DebugView / WinDbg, Kd_IHVAUDIO_Mask */
#include <wdm.h>
#define OZZY_LOG_LINE(prefix, fmt, ...) \
	DbgPrintEx(DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, prefix fmt "\n", ##__VA_ARGS__)
#elif defined(OZZY_USER_ENGINE)
/* WinUSB harness: stderr */
#include <stdio.h>
#define OZZY_LOG_LINE(prefix, fmt, ...) fprintf(stderr, prefix fmt "\n", ##__VA_ARGS__)
#elif defined(KERNEL)
/* macOS kext */
#include <IOKit/IOLib.h>
#define OZZY_LOG_LINE(prefix, fmt, ...) IOLog(prefix fmt "\n", ##__VA_ARGS__)
#define OZZY_LOG_PREFIX "[OzzyKext] "   /* what macos/debuglogs.command filters on */
#else
#error "ozzy_log.h is for engine builds (kext, Windows driver, harness)"
#endif

#ifndef OZZY_LOG_PREFIX
#define OZZY_LOG_PREFIX "[Ozzy] "
#endif

#define ozzy_log(fmt, ...)      OZZY_LOG_LINE(OZZY_LOG_PREFIX, fmt, ##__VA_ARGS__)
#define ploytec_log(fmt, ...)   OZZY_LOG_LINE("[Ploytec] ", fmt, ##__VA_ARGS__)
#define usb_midi_log(fmt, ...)  OZZY_LOG_LINE("[UsbMidi] ", fmt, ##__VA_ARGS__)

#endif /* OZZY_LOG_H */
