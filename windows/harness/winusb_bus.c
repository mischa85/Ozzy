#include "winusb_bus.h"
#include <avrt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static const ULONG_PTR quit_key = 1;   /* completion key of the packet that stops the thread */

struct wbus_transfer {
    OVERLAPPED ov;        /* first: the completion port hands back &ov */
    uint8_t pipe;
    void* xfer;
};

static int wbus_pipe_slot(uint8_t address) {
    return (address & 0x0F) | ((address & 0x80) >> 3);
}

/* Run the calling thread as a real-time audio thread (MMCSS "Pro Audio", time critical) */
static void make_audio_thread(void) {
    DWORD task = 0;
    AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}

void wbus_log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

static uint64_t wbus_time(void* ctx) {
    (void)ctx;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
}

static void wbus_sleep(void* ctx, unsigned int ms) {
    (void)ctx;
    Sleep(ms);
}

static bool wbus_submit(void* ctx, uint8_t pipe, void* buf, uint32_t size, void* xfer) {
    struct winusb_bus* wb = ctx;
    int slot = wbus_pipe_slot(pipe);
    WINUSB_INTERFACE_HANDLE h = wb->pipe_intf[slot];
    if (!h || wb->pipe_addr[slot] != pipe) return false;

    struct wbus_transfer* t = calloc(1, sizeof(*t));
    if (!t) return false;
    t->pipe = pipe;
    t->xfer = xfer;
    InterlockedIncrement(&wb->pending);
    BOOL ok = (pipe & 0x80) ? WinUsb_ReadPipe(h, pipe, (PUCHAR)buf, size, NULL, &t->ov)
                            : WinUsb_WritePipe(h, pipe, (PUCHAR)buf, size, NULL, &t->ov);
    /* Pending or done: either way the completion comes through the port */
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
        wbus_log("[WinUsbBus] pipe 0x%02x submit failed: %lu", pipe, GetLastError());
        InterlockedDecrement(&wb->pending);
        free(t);
        return false;
    }
    return true;
}

static bool wbus_control(void* ctx, uint8_t type, uint8_t req, uint16_t val, uint16_t idx, void* data,
                         uint16_t len) {
    struct winusb_bus* wb = ctx;
    WINUSB_SETUP_PACKET setup = { type, req, val, idx, len };
    ULONG done = 0;
    if (WinUsb_ControlTransfer(wb->intf[0], setup, (PUCHAR)data, len, &done, NULL)) return true;
    wbus_log("[WinUsbBus] control request 0x%02x (type 0x%02x, wValue 0x%04x) failed: %lu", req, type, val,
             GetLastError());
    return false;
}

/* Clears the stall on the device's endpoint and the pipe's data toggle */
static int wbus_clear_halt(void* ctx, uint8_t address) {
    struct winusb_bus* wb = ctx;
    int slot = wbus_pipe_slot(address);
    if (!wb->pipe_intf[slot] || wb->pipe_addr[slot] != address) return -ENODEV;
    if (WinUsb_ResetPipe(wb->pipe_intf[slot], address)) return 0;
    wbus_log("[WinUsbBus] clear halt on pipe 0x%02x failed: %lu", address, GetLastError());
    return -EIO;
}

/* Deliver completions to the engine, one at a time. A failed transfer still completes through
 * the port: GetQueuedCompletionStatus returns FALSE with its OVERLAPPED, and the error (an
 * aborted pipe is ERROR_OPERATION_ABORTED, which the engines know as OZZY_STATUS_ABORTED). */
static DWORD WINAPI wbus_completion_thread(void* arg) {
    struct winusb_bus* wb = arg;
    make_audio_thread();
    for (;;) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* ov = NULL;
        BOOL ok = GetQueuedCompletionStatus(wb->port, &bytes, &key, &ov, INFINITE);
        if (!ov) {
            if (!ok || key == quit_key) break;
            continue;
        }
        struct wbus_transfer* t = CONTAINING_RECORD(ov, struct wbus_transfer, ov);
        int status = ok ? 0 : (int)GetLastError();
        uint8_t pipe = t->pipe;
        void* xfer = t->xfer;
        free(t);

        if (status == 0) InterlockedIncrement((pipe & 0x80) ? &wb->in_done : &wb->out_done);
        else if (status != ERROR_OPERATION_ABORTED && InterlockedIncrement(&wb->errors) <= 10)
            wbus_log("[WinUsbBus] pipe 0x%02x error: %d", pipe, status);

        if (wb->engine) ozzy_engine_complete(wb->engine, pipe, xfer, status, bytes);
        InterlockedDecrement(&wb->pending);
    }
    return 0;
}

/* ozzy_engine_timer every interval, on absolute deadlines (no drift); if we fell behind,
 * skip to the next deadline ahead, as KextBus does */
static DWORD WINAPI wbus_timer_thread(void* arg) {
    struct winusb_bus* wb = arg;
    make_audio_thread();
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    if (!timer) {
        wbus_log("[WinUsbBus] no timer: %lu", GetLastError());
        return 0;
    }
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    int64_t interval = (int64_t)wb->timer_interval_us * freq.QuadPart / 1000000;
    int64_t deadline = (int64_t)wbus_time(wb) + interval;

    while (InterlockedCompareExchange(&wb->timer_on, 0, 0)) {
        int64_t now = (int64_t)wbus_time(wb);
        if (deadline > now) {
            LARGE_INTEGER due;
            due.QuadPart = -((deadline - now) * 10000000 / freq.QuadPart);   /* relative, 100 ns */
            if (due.QuadPart == 0) due.QuadPart = -1;
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(timer, INFINITE);
        }
        if (!InterlockedCompareExchange(&wb->timer_on, 0, 0)) break;
        if (wb->engine) ozzy_engine_timer(wb->engine);
        deadline += interval;
        now = (int64_t)wbus_time(wb);
        if (deadline <= now) deadline = now + interval;
    }
    CloseHandle(timer);
    return 0;
}

static bool wbus_start_timer(void* ctx, uint32_t interval_us) {
    struct winusb_bus* wb = ctx;
    if (wb->timer_thread) return true;
    wb->timer_interval_us = interval_us;
    InterlockedExchange(&wb->timer_on, 1);
    wb->timer_thread = CreateThread(NULL, 0, wbus_timer_thread, wb, 0, NULL);
    if (!wb->timer_thread) InterlockedExchange(&wb->timer_on, 0);
    return wb->timer_thread != NULL;
}

static void wbus_stop_timer(void* ctx) {
    struct winusb_bus* wb = ctx;
    InterlockedExchange(&wb->timer_on, 0);
    if (!wb->timer_thread) return;
    WaitForSingleObject(wb->timer_thread, INFINITE);   /* waits for a call in progress */
    CloseHandle(wb->timer_thread);
    wb->timer_thread = NULL;
}

/* Abort what is in flight and wait until the completion thread has delivered it */
static void wbus_abort(void* ctx) {
    struct winusb_bus* wb = ctx;
    for (int s = 0; s < WBUS_PIPE_SLOTS; s++) {
        if (wb->pipe_intf[s]) WinUsb_AbortPipe(wb->pipe_intf[s], wb->pipe_addr[s]);
    }
    for (int i = 0; i < 2000 && InterlockedCompareExchange(&wb->pending, 0, 0) > 0; i++) Sleep(1);
    LONG pending = InterlockedCompareExchange(&wb->pending, 0, 0);
    if (pending > 0) wbus_log("[WinUsbBus] %ld transfers still pending after abort", pending);
}

/* No isochronous transfers (WinUSB can't give each packet its own length) and no PCM rings
 * (the harness plays the HAL's part of the shared memory) */
static const struct ozzy_bus_ops wbus_ops = {
    .submit = wbus_submit,
    .control = wbus_control,
    .clear_halt = wbus_clear_halt,
    .start_timer = wbus_start_timer,
    .stop_timer = wbus_stop_timer,
    .sleep_ms = wbus_sleep,
    .time = wbus_time,
    .abort = wbus_abort,
};

bool wbus_open(struct winusb_bus* wb, const wchar_t* path, struct ozzy_engine* engine) {
    wb->bus.ops = &wbus_ops;
    wb->bus.ctx = wb;
    wb->engine = engine;
    wb->file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (wb->file == INVALID_HANDLE_VALUE) {
        wbus_log("[WinUsbBus] opening the device failed: %lu", GetLastError());
        return false;
    }
    if (!WinUsb_Initialize(wb->file, &wb->intf[0])) {
        wbus_log("[WinUsbBus] WinUsb_Initialize failed: %lu", GetLastError());
        return false;
    }

    /* Every transfer completes through this port, on one thread */
    wb->port = CreateIoCompletionPort(wb->file, NULL, 0, 1);
    if (!wb->port) {
        wbus_log("[WinUsbBus] no completion port: %lu", GetLastError());
        return false;
    }

    ULONG len = 0;
    if (!WinUsb_GetDescriptor(wb->intf[0], USB_DEVICE_DESCRIPTOR_TYPE, 0, 0, (PUCHAR)&wb->device_desc,
                              sizeof(wb->device_desc), &len)) {
        wbus_log("[WinUsbBus] no device descriptor: %lu", GetLastError());
        return false;
    }

    /* The first interface comes from WinUsb_Initialize, the others are "associated" with it */
    USB_INTERFACE_DESCRIPTOR d;
    if (WinUsb_QueryInterfaceSettings(wb->intf[0], 0, &d)) wb->intf_number[0] = d.bInterfaceNumber;
    wb->intf_count = 1;
    for (UCHAR i = 0; wb->intf_count < WBUS_MAX_INTERFACES; i++) {
        WINUSB_INTERFACE_HANDLE h;
        if (!WinUsb_GetAssociatedInterface(wb->intf[0], i, &h)) break;   /* ERROR_NO_MORE_ITEMS */
        wb->intf[wb->intf_count] = h;
        wb->intf_number[wb->intf_count] = WinUsb_QueryInterfaceSettings(h, 0, &d) ? d.bInterfaceNumber : 0xFF;
        wb->intf_count++;
    }

    wb->completion_thread = CreateThread(NULL, 0, wbus_completion_thread, wb, 0, NULL);
    return wb->completion_thread != NULL;
}

void wbus_close(struct winusb_bus* wb) {
    wbus_stop_timer(wb);

    /* Anything still in flight (ozzy_engine_stop has aborted what the engine had) */
    wbus_abort(wb);

    if (wb->completion_thread) {
        PostQueuedCompletionStatus(wb->port, 0, quit_key, NULL);
        WaitForSingleObject(wb->completion_thread, INFINITE);
        CloseHandle(wb->completion_thread);
        wb->completion_thread = NULL;
    }
    for (int s = 0; s < WBUS_PIPE_SLOTS; s++) wb->pipe_intf[s] = NULL;
    for (int i = wb->intf_count - 1; i >= 0; i--) {
        if (wb->intf[i]) WinUsb_Free(wb->intf[i]);
        wb->intf[i] = NULL;
    }
    wb->intf_count = 0;
    if (wb->port) {
        CloseHandle(wb->port);
        wb->port = NULL;
    }
    if (wb->file && wb->file != INVALID_HANDLE_VALUE) CloseHandle(wb->file);
    wb->file = NULL;
}

bool wbus_configure(struct winusb_bus* wb) {
    const struct ozzy_device_profile* profile = &wb->engine->profile;

    /* The profile's interfaces, with its alternate setting */
    WINUSB_INTERFACE_HANDLE by_index[4] = { 0 };
    for (int i = 0; i < profile->interface_count; i++) {
        for (int k = 0; k < wb->intf_count; k++) {
            if (wb->intf_number[k] == profile->interfaces[i]) by_index[i] = wb->intf[k];
        }
        if (!by_index[i]) {
            wbus_log("[WinUsbBus] interface %u not found (is WinUSB bound to the whole device, not one interface?)",
                     profile->interfaces[i]);
            return false;
        }
        if (profile->alt_setting) {
            if (!WinUsb_SetCurrentAlternateSetting(by_index[i], profile->alt_setting)) {
                wbus_log("[WinUsbBus] interface %u: alternate setting %u failed: %lu", profile->interfaces[i],
                         profile->alt_setting, GetLastError());
                return false;
            }
            Sleep(50);
        }
    }

    /* Every endpoint of the claimed interfaces to the engine, which picks its pipes */
    struct ozzy_usb_endpoint eps[32];
    unsigned int n = 0;
    for (int i = 0; i < profile->interface_count; i++) {
        UCHAR alt = 0;
        USB_INTERFACE_DESCRIPTOR d;
        WinUsb_GetCurrentAlternateSetting(by_index[i], &alt);
        if (!WinUsb_QueryInterfaceSettings(by_index[i], alt, &d)) continue;
        for (UCHAR e = 0; e < d.bNumEndpoints && n < 32; e++) {
            WINUSB_PIPE_INFORMATION_EX pipe;
            if (!WinUsb_QueryPipeEx(by_index[i], alt, e, &pipe)) continue;
            eps[n].intf = profile->interfaces[i];
            eps[n].address = pipe.PipeId;
            eps[n].attributes = (uint8_t)pipe.PipeType;   /* USBD_PIPE_TYPE is bmAttributes & 3 */
            eps[n].interval = pipe.Interval;
            eps[n].max_packet = pipe.MaximumPacketSize;
            eps[n++].handle = by_index[i];
        }
    }
    const struct ozzy_usb_endpoint* bound[OZZY_PIPE_ROLES];
    ozzy_engine_bind_endpoints(wb->engine, eps, n, bound);
    for (int role = 0; role < OZZY_PIPE_ROLES; role++) {
        if (!bound[role]) continue;
        int slot = wbus_pipe_slot(bound[role]->address);
        wb->pipe_intf[slot] = bound[role]->handle;
        wb->pipe_addr[slot] = bound[role]->address;
    }
    return true;
}

bool wbus_get_string(struct winusb_bus* wb, uint8_t index, char* dst, size_t size) {
    UCHAR buf[256];
    ULONG len = 0;
    if (!size) return false;
    dst[0] = 0;
    if (!index ||
        !WinUsb_GetDescriptor(wb->intf[0], USB_STRING_DESCRIPTOR_TYPE, index, 0x0409, buf, sizeof(buf), &len) ||
        len < 2)
        return false;
    size_t chars = ((len < buf[0] ? len : buf[0]) - 2) / 2, i;
    for (i = 0; i < chars && i + 1 < size; i++) {
        uint16_t c = (uint16_t)(buf[2 + 2 * i] | (buf[3 + 2 * i] << 8));
        dst[i] = c < 0x80 ? (char)c : '?';
    }
    dst[i] = 0;
    return true;
}

struct wbus_stats wbus_take_stats(struct winusb_bus* wb) {
    struct wbus_stats s = {
        (uint32_t)InterlockedExchange(&wb->out_done, 0),
        (uint32_t)InterlockedExchange(&wb->in_done, 0),
        (uint32_t)InterlockedExchange(&wb->errors, 0),
    };
    return s;
}
