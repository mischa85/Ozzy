/* SPDX-License-Identifier: MIT */
/*
 * Engine logging: one line per call, with the component as prefix, wherever the engine runs.
 * (The macOS HAL and MIDI plugins log through macos/Shared/OzzyLog.h, the rest of the Linux
 * module through linux/ozzy_log.h, which takes a device; don't include both.)
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_ENGINE_LOG_H
#define OZZY_ENGINE_LOG_H

#if defined(__KERNEL__) && defined(__linux__)
/* Linux module: the kernel log, with the prefixes linux/ozzy_log.h uses */
#include <linux/printk.h>
#include <linux/stdarg.h>
#define OZZY_LOG_LINE(prefix, fmt, ...) pr_info(prefix fmt "\n", ##__VA_ARGS__)
#define OZZY_LOG_PREFIX "[ozzy] "
#define PLOYTEC_LOG_PREFIX "[ploytec] "
#define USB_MIDI_LOG_PREFIX "[usb-midi] "
#elif defined(_KERNEL_MODE)
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
#ifndef PLOYTEC_LOG_PREFIX
#define PLOYTEC_LOG_PREFIX "[Ploytec] "
#endif
#ifndef USB_MIDI_LOG_PREFIX
#define USB_MIDI_LOG_PREFIX "[UsbMidi] "
#endif

#define ozzy_log(fmt, ...)      OZZY_LOG_LINE(OZZY_LOG_PREFIX, fmt, ##__VA_ARGS__)
#define ploytec_log(fmt, ...)   OZZY_LOG_LINE(PLOYTEC_LOG_PREFIX, fmt, ##__VA_ARGS__)
#define usb_midi_log(fmt, ...)  OZZY_LOG_LINE(USB_MIDI_LOG_PREFIX, fmt, ##__VA_ARGS__)

/* ploytec_log() of a va_list (the handshake's vlog) */
#if defined(__KERNEL__) && defined(__linux__)
static inline void ploytec_vlog(const char *fmt, va_list args)
{
	struct va_format vaf;
	va_list copy;

	/* va_list parameters decay to a pointer on some ABIs; %pV needs a real va_list */
	va_copy(copy, args);
	vaf.fmt = fmt;
	vaf.va = &copy;
	pr_info(PLOYTEC_LOG_PREFIX "%pV\n", &vaf);
	va_end(copy);
}
#else
#include "ozzy_platform.h"
static inline OZZY_PRINTF_FORMAT(1, 0) void ploytec_vlog(const char *fmt, va_list args)
{
	char line[256];

	ozzy_vsnprintf(line, sizeof(line), fmt, args);
	ploytec_log("%s", line);
}
#endif

#endif /* OZZY_ENGINE_LOG_H */
