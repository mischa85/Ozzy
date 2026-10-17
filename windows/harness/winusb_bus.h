#ifndef WINUSB_BUS_H
#define WINUSB_BUS_H

/* The engine's bus (engine/core/ozzy_bus.h) over WinUSB, for the Windows dev harness: the
 * engine runs in a console program and talks to a device bound to WinUsb.sys (e.g. with
 * Zadig). Not shipped; the driver is ACX.
 *
 * Transfers are overlapped I/O on the device handle, completed on one thread through an I/O
 * completion port, so engine callbacks never run concurrently (as on the kext's work loop).
 * Isochronous OUT is not possible: WinUSB packetizes a write at the full bytes per interval
 * and takes no per-packet lengths, which feedback pacing needs (the ACX driver uses URBs). */

#include <windows.h>
#include <winusb.h>
#include <usb.h>
#include "../../engine/core/ozzy_engine.h"

enum {
    WBUS_MAX_INTERFACES = 8,
    WBUS_PIPE_SLOTS = 32,   /* endpoint number in bits 0-3, direction in bit 4 (as KextBus) */
};

/* Zero it, then wbus_open */
struct winusb_bus {
    struct ozzy_bus bus;    /* what the engine runs on (ozzy_engine_attach) */
    struct ozzy_engine* engine;

    HANDLE file;
    HANDLE port;
    WINUSB_INTERFACE_HANDLE intf[WBUS_MAX_INTERFACES];   /* [0] from WinUsb_Initialize */
    uint8_t intf_number[WBUS_MAX_INTERFACES];           /* bInterfaceNumber of each handle */
    int intf_count;
    WINUSB_INTERFACE_HANDLE pipe_intf[WBUS_PIPE_SLOTS];  /* interface handle per endpoint */
    uint8_t pipe_addr[WBUS_PIPE_SLOTS];
    USB_DEVICE_DESCRIPTOR device_desc;

    HANDLE completion_thread;
    volatile LONG pending;
    volatile LONG out_done, in_done, errors;

    HANDLE timer_thread;
    volatile LONG timer_on;
    uint32_t timer_interval_us;
};

/* Completed transfers and failures per direction since the last call (harness stats) */
struct wbus_stats {
    uint32_t out_done, in_done, errors;
};

/* Open the device at @path (a WinUSB device interface path) and start the completion thread */
bool wbus_open(struct winusb_bus* wb, const wchar_t* path, struct ozzy_engine* engine);
/* Abort every pipe, wait for outstanding transfers, close everything. Safe to call twice. */
void wbus_close(struct winusb_bus* wb);

/* What OzzyKext::ConfigureHardware does: claim the profile's interfaces, select the
 * alternate setting, find each pipe and report it with ozzy_engine_set_endpoint */
bool wbus_configure(struct winusb_bus* wb);

/* String descriptor @index (US English) as ASCII into @dst; false if there is none */
bool wbus_get_string(struct winusb_bus* wb, uint8_t index, char* dst, size_t size);

struct wbus_stats wbus_take_stats(struct winusb_bus* wb);

void wbus_log(const char* fmt, ...);

#endif
