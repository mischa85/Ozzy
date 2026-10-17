#include "kmdf_bus.h"
#include "stream.h"
#include <ntstrsafe.h>
#include <stdarg.h>

#define OZZY_POOL_TAG 'yzzO'

static EVT_WDF_REQUEST_COMPLETION_ROUTINE kbus_completion;
static EXT_CALLBACK kbus_timer_callback;

static uint64_t kbus_time(void* ctx) {
    UNREFERENCED_PARAMETER(ctx);
    return (uint64_t)KeQueryPerformanceCounter(NULL).QuadPart;
}

static void kbus_sleep(void* ctx, unsigned int ms) {
    UNREFERENCED_PARAMETER(ctx);
    LARGE_INTEGER delay;
    delay.QuadPart = -(LONGLONG)ms * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &delay);
}

void kbus_log(const char* fmt, ...) {
    char line[256];
    va_list args;
    va_start(args, fmt);
    RtlStringCbVPrintfA(line, sizeof(line), fmt, args);
    va_end(args);
    DbgPrintEx(DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "%s\n", line);
}

static struct kbus_pipe* kbus_find_pipe(struct kmdf_bus* kb, uint8_t address) {
    for (int i = 0; i < kb->pipe_count; i++) {
        if (kb->pipes[i].address == address) return &kb->pipes[i];
    }
    return NULL;
}

static bool kbus_submit(void* ctx, uint8_t address, void* buf, uint32_t size, void* xfer) {
    struct kmdf_bus* kb = ctx;
    struct kbus_pipe* p = kbus_find_pipe(kb, address);
    if (!p || !kb->shm) return false;

    /* A free request: claim its bit */
    int i;
    for (;;) {
        LONG busy = p->busy;
        for (i = 0; i < KBUS_REQUESTS_PER_PIPE && (busy & (1 << i)); i++) {}
        if (i == KBUS_REQUESTS_PER_PIPE) return false;
        if (InterlockedCompareExchange(&p->busy, busy | (1 << i), busy) == busy) break;
    }
    WDFREQUEST request = p->requests[i];

    WDF_REQUEST_REUSE_PARAMS reuse;
    WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
    WdfRequestReuse(request, &reuse);

    /* Zero copy: the request points into the shared memory */
    WDFMEMORY_OFFSET offset = { (size_t)((uint8_t*)buf - kb->shm_base), size };
    NTSTATUS status = (address & 0x80) ? WdfUsbTargetPipeFormatRequestForRead(p->pipe, request, kb->shm, &offset)
                                       : WdfUsbTargetPipeFormatRequestForWrite(p->pipe, request, kb->shm, &offset);
    if (NT_SUCCESS(status)) {
        p->slots[i].xfer = xfer;
        WdfRequestSetCompletionRoutine(request, kbus_completion, &p->slots[i]);
        if (WdfRequestSend(request, WdfUsbTargetPipeGetIoTarget(p->pipe), WDF_NO_SEND_OPTIONS)) return true;
        status = WdfRequestGetStatus(request);
    }
    InterlockedAnd(&p->busy, ~(1 << i));
    if (status != STATUS_CANCELLED) kbus_log("[Ozzy] pipe 0x%02x submit failed: 0x%08x", address, status);
    return false;
}

/* DISPATCH_LEVEL: release the request, then hand the result to the engine, which resubmits and
 * moves PCM through its rings from here */
static void kbus_completion(WDFREQUEST request, WDFIOTARGET target, PWDF_REQUEST_COMPLETION_PARAMS params,
                            WDFCONTEXT context) {
    UNREFERENCED_PARAMETER(request);
    UNREFERENCED_PARAMETER(target);
    struct kbus_slot* slot = context;
    struct kmdf_bus* kb = slot->bus;
    struct kbus_pipe* p = &kb->pipes[slot->pipe];
    void* xfer = slot->xfer;
    NTSTATUS status = params->IoStatus.Status;
    ULONG bytes = (p->address & 0x80) ? params->Parameters.Usb.Completion->Parameters.PipeRead.Length
                                      : params->Parameters.Usb.Completion->Parameters.PipeWrite.Length;

    InterlockedAnd(&p->busy, ~(1 << (int)(slot - p->slots)));

    InterlockedIncrement(&kb->in_completion);
    if (!NT_SUCCESS(status) && status != STATUS_CANCELLED)
        DbgPrintEx(DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "[Ozzy] pipe 0x%02x error: 0x%08x\n", p->address, status);
    if (kb->engine) ozzy_engine_complete(kb->engine, p->address, xfer, (int)status, bytes);
    InterlockedDecrement(&kb->in_completion);
}

/* PASSIVE_LEVEL (the handshake), 5 s timeout like the kext's deviceRequest */
static bool kbus_control(void* ctx, uint8_t type, uint8_t req, uint16_t val, uint16_t idx, void* data,
                         uint16_t len) {
    struct kmdf_bus* kb = ctx;
    WDF_USB_CONTROL_SETUP_PACKET setup;
    RtlZeroMemory(&setup, sizeof(setup));
    setup.Packet.bm.Byte = type;
    setup.Packet.bRequest = req;
    setup.Packet.wValue.Value = val;
    setup.Packet.wIndex.Value = idx;
    setup.Packet.wLength = len;

    WDF_MEMORY_DESCRIPTOR memory;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&memory, data, len);
    WDF_REQUEST_SEND_OPTIONS options;
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_SEC(5));

    ULONG done = 0;
    NTSTATUS status = WdfUsbTargetDeviceSendControlTransferSynchronously(kb->usb, WDF_NO_HANDLE, &options, &setup,
                                                                        len ? &memory : NULL, &done);
    if (NT_SUCCESS(status)) return true;
    kbus_log("[Ozzy] control request 0x%02x (type 0x%02x, wValue 0x%04x) failed: 0x%08x", req, type, val, status);
    return false;
}

/* PASSIVE_LEVEL (the handshake): CLEAR_FEATURE(ENDPOINT_HALT) and a reset of the pipe */
static int kbus_clear_halt(void* ctx, uint8_t address) {
    struct kbus_pipe* p = kbus_find_pipe(ctx, address);
    if (!p) return -ENODEV;
    NTSTATUS status = WdfUsbTargetPipeResetSynchronously(p->pipe, WDF_NO_HANDLE, NULL);
    if (NT_SUCCESS(status)) return 0;
    kbus_log("[Ozzy] clear halt on pipe 0x%02x failed: 0x%08x", address, status);
    return -EIO;
}

static void kbus_timer_callback(PEX_TIMER timer, PVOID context) {
    UNREFERENCED_PARAMETER(timer);
    struct kmdf_bus* kb = context;
    if (kb->engine) ozzy_engine_timer(kb->engine);
}

static bool kbus_start_timer(void* ctx, uint32_t interval_us) {
    struct kmdf_bus* kb = ctx;
    if (!kb->timer) kb->timer = ExAllocateTimer(kbus_timer_callback, kb, EX_TIMER_HIGH_RESOLUTION);
    if (!kb->timer) return false;
    LONGLONG period = (LONGLONG)interval_us * 10;   /* 100 ns units */
    ExSetTimer(kb->timer, -period, period, NULL);
    return true;
}

static void kbus_stop_timer(void* ctx) {
    struct kmdf_bus* kb = ctx;
    if (!kb->timer) return;
    /* Waits for a callback in progress */
    ExDeleteTimer(kb->timer, TRUE, TRUE, NULL);
    kb->timer = NULL;
}

static void kbus_pcm_progress(void* ctx, enum ozzy_pcm_dir dir, uint64_t frames) {
    struct kmdf_bus* kb = ctx;
    struct wstream* stream = InterlockedCompareExchangePointer((PVOID volatile*)&kb->stream[dir], NULL, NULL);
    if (stream) wstream_progress(stream, frames, kbus_time(kb));
}

/* Isochronous URBs (per-packet lengths, ring wrap) come with the Ploytec isochronous family */
static const struct ozzy_bus_ops kbus_ops = {
    .submit = kbus_submit,
    .control = kbus_control,
    .clear_halt = kbus_clear_halt,
    .start_timer = kbus_start_timer,
    .stop_timer = kbus_stop_timer,
    .pcm_progress = kbus_pcm_progress,
    .sleep_ms = kbus_sleep,
    .time = kbus_time,
};

void kbus_init(struct kmdf_bus* kb, struct ozzy_engine* engine) {
    kb->bus.ops = &kbus_ops;
    kb->bus.ctx = kb;
    kb->engine = engine;
}

void kbus_release(struct kmdf_bus* kb) {
    kbus_stop_timer(kb);
    for (int i = 0; i < kb->pipe_count; i++) {
        for (int k = 0; k < KBUS_REQUESTS_PER_PIPE; k++) {
            if (kb->pipes[i].requests[k]) WdfObjectDelete(kb->pipes[i].requests[k]);
        }
    }
    if (kb->shm) WdfObjectDelete(kb->shm);
    if (kb->usb) WdfObjectDelete(kb->usb);
    if (kb->config) ExFreePoolWithTag(kb->config, OZZY_POOL_TAG);
    RtlZeroMemory(kb, sizeof(*kb));
}

NTSTATUS kbus_create(struct kmdf_bus* kb, WDFDEVICE device) {
    kb->device = device;

    WDF_USB_DEVICE_CREATE_CONFIG config;
    WDF_USB_DEVICE_CREATE_CONFIG_INIT(&config, USBD_CLIENT_CONTRACT_VERSION_602);
    NTSTATUS status = WdfUsbTargetDeviceCreateWithParameters(device, &config, WDF_NO_OBJECT_ATTRIBUTES, &kb->usb);
    if (!NT_SUCCESS(status)) {
        kbus_log("[Ozzy] USB target: 0x%08x", status);
        return status;
    }

    /* Configuration 1 with every interface at setting 0; kbus_configure selects the profile's setting */
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS params;
    if (WdfUsbTargetDeviceGetNumInterfaces(kb->usb) == 1)
        WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_SINGLE_INTERFACE(&params);
    else
        WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_MULTIPLE_INTERFACES(&params, 0, NULL);
    status = WdfUsbTargetDeviceSelectConfig(kb->usb, WDF_NO_OBJECT_ATTRIBUTES, &params);
    if (!NT_SUCCESS(status)) {
        kbus_log("[Ozzy] select configuration: 0x%08x", status);
        return status;
    }

    /* The raw configuration descriptor: bmAttributes and wMaxPacketSize as the device has them */
    USHORT size = 0;
    WdfUsbTargetDeviceRetrieveConfigDescriptor(kb->usb, NULL, &size);
    kb->config = ExAllocatePool2(POOL_FLAG_NON_PAGED, size, OZZY_POOL_TAG);
    if (!kb->config) return STATUS_INSUFFICIENT_RESOURCES;
    return WdfUsbTargetDeviceRetrieveConfigDescriptor(kb->usb, kb->config, &size);
}

/* Endpoint @address of interface @intf, alternate setting @alt, in the configuration descriptor */
static const USB_ENDPOINT_DESCRIPTOR* kbus_find_endpoint(const struct kmdf_bus* kb, uint8_t intf, uint8_t alt,
                                                         uint8_t address) {
    const uint8_t* p = (const uint8_t*)kb->config;
    const uint8_t* end = p + kb->config->wTotalLength;
    bool in_setting = false;
    while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end) {
        if (p[1] == USB_INTERFACE_DESCRIPTOR_TYPE && p[0] >= sizeof(USB_INTERFACE_DESCRIPTOR)) {
            const USB_INTERFACE_DESCRIPTOR* d = (const USB_INTERFACE_DESCRIPTOR*)p;
            in_setting = d->bInterfaceNumber == intf && d->bAlternateSetting == alt;
        } else if (in_setting && p[1] == USB_ENDPOINT_DESCRIPTOR_TYPE && p[0] >= sizeof(USB_ENDPOINT_DESCRIPTOR)) {
            const USB_ENDPOINT_DESCRIPTOR* e = (const USB_ENDPOINT_DESCRIPTOR*)p;
            if (e->bEndpointAddress == address) return e;
        }
        p += p[0];
    }
    return NULL;
}

/* The pipe's requests, created once and reused for every transfer */
static NTSTATUS kbus_add_pipe(struct kmdf_bus* kb, WDFUSBPIPE pipe, uint8_t address) {
    if (kb->pipe_count == KBUS_MAX_PIPES) return STATUS_INSUFFICIENT_RESOURCES;
    struct kbus_pipe* p = &kb->pipes[kb->pipe_count];
    p->pipe = pipe;
    p->address = address;
    p->busy = 0;
    if (address & 0x80) WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(pipe);

    for (int i = 0; i < KBUS_REQUESTS_PER_PIPE; i++) {
        WDF_OBJECT_ATTRIBUTES attributes;
        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
        attributes.ParentObject = kb->device;
        NTSTATUS status = WdfRequestCreate(&attributes, WdfUsbTargetPipeGetIoTarget(pipe), &p->requests[i]);
        if (!NT_SUCCESS(status)) return status;
        p->slots[i].bus = kb;
        p->slots[i].pipe = (uint8_t)kb->pipe_count;
        p->slots[i].xfer = NULL;
    }
    kb->pipe_count++;
    return STATUS_SUCCESS;
}

NTSTATUS kbus_configure(struct kmdf_bus* kb, void* shm, size_t shm_size) {
    const struct ozzy_device_profile* profile = &kb->engine->profile;

    kb->shm_base = shm;
    NTSTATUS status = WdfMemoryCreatePreallocated(WDF_NO_OBJECT_ATTRIBUTES, shm, shm_size, &kb->shm);
    if (!NT_SUCCESS(status)) return status;

    /* The profile's interfaces, with its alternate setting */
    WDFUSBINTERFACE by_index[4] = { 0 };
    UCHAR count = WdfUsbTargetDeviceGetNumInterfaces(kb->usb);
    for (int i = 0; i < profile->interface_count; i++) {
        for (UCHAR k = 0; k < count; k++) {
            WDFUSBINTERFACE intf = WdfUsbTargetDeviceGetInterface(kb->usb, k);
            if (WdfUsbInterfaceGetInterfaceNumber(intf) == profile->interfaces[i]) by_index[i] = intf;
        }
        if (!by_index[i]) {
            kbus_log("[Ozzy] interface %u not found", profile->interfaces[i]);
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        if (profile->alt_setting) {
            WDF_USB_INTERFACE_SELECT_SETTING_PARAMS params;
            WDF_USB_INTERFACE_SELECT_SETTING_PARAMS_INIT_SETTING(&params, profile->alt_setting);
            status = WdfUsbInterfaceSelectSetting(by_index[i], WDF_NO_OBJECT_ATTRIBUTES, &params);
            if (!NT_SUCCESS(status)) {
                kbus_log("[Ozzy] interface %u: alternate setting %u: 0x%08x", profile->interfaces[i],
                         profile->alt_setting, status);
                return status;
            }
        }
    }

    /* Every endpoint of the claimed interfaces to the engine, which picks its pipes */
    struct ozzy_usb_endpoint eps[32];
    unsigned int n = 0;
    for (int i = 0; i < profile->interface_count; i++) {
        uint8_t alt = WdfUsbInterfaceGetConfiguredSettingIndex(by_index[i]);
        BYTE pipes = WdfUsbInterfaceGetNumConfiguredPipes(by_index[i]);
        for (BYTE j = 0; j < pipes && n < 32; j++) {
            WDF_USB_PIPE_INFORMATION info;
            WDF_USB_PIPE_INFORMATION_INIT(&info);
            WDFUSBPIPE pipe = WdfUsbInterfaceGetConfiguredPipe(by_index[i], j, &info);
            const USB_ENDPOINT_DESCRIPTOR* e =
                kbus_find_endpoint(kb, profile->interfaces[i], alt, info.EndpointAddress);
            if (!pipe || !e) continue;
            eps[n].intf = profile->interfaces[i];
            eps[n].address = e->bEndpointAddress;
            eps[n].attributes = e->bmAttributes;
            eps[n].interval = e->bInterval;
            eps[n].max_packet = e->wMaxPacketSize;
            eps[n++].handle = pipe;
        }
    }
    const struct ozzy_usb_endpoint* bound[OZZY_PIPE_ROLES];
    ozzy_engine_bind_endpoints(kb->engine, eps, n, bound);
    for (int role = 0; role < OZZY_PIPE_ROLES; role++) {
        if (!bound[role]) continue;
        status = kbus_add_pipe(kb, (WDFUSBPIPE)bound[role]->handle, bound[role]->address);
        if (!NT_SUCCESS(status)) return status;
    }
    return STATUS_SUCCESS;
}

void kbus_stop_io(struct kmdf_bus* kb) {
    for (int i = 0; i < kb->pipe_count; i++)
        WdfIoTargetStop(WdfUsbTargetPipeGetIoTarget(kb->pipes[i].pipe), WdfIoTargetCancelSentIo);
}

void kbus_start_io(struct kmdf_bus* kb) {
    for (int i = 0; i < kb->pipe_count; i++) WdfIoTargetStart(WdfUsbTargetPipeGetIoTarget(kb->pipes[i].pipe));
}

void kbus_set_stream(struct kmdf_bus* kb, enum ozzy_pcm_dir dir, struct wstream* stream) {
    InterlockedExchangePointer((PVOID volatile*)&kb->stream[dir], stream);
}

/* PASSIVE_LEVEL. A completion that started before a detach may still use what was detached;
 * once none is in progress, every later one sees the detach. */
void kbus_wait_completions(struct kmdf_bus* kb) {
    for (int i = 0; i < 200 && InterlockedCompareExchange(&kb->in_completion, 0, 0) != 0; i++) kbus_sleep(kb, 1);
    KeFlushQueuedDpcs();
}
