#include "device.h"
#include "../../common/ozzy_version.h"

static EVT_WDF_DEVICE_PREPARE_HARDWARE wdev_evt_prepare_hardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE wdev_evt_release_hardware;
static EVT_WDF_DEVICE_D0_ENTRY wdev_evt_d0_entry;
static EVT_WDF_DEVICE_D0_EXIT wdev_evt_d0_exit;

/* Four hex digits at @s, or -1 */
static int hex4(const WCHAR* s) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        WCHAR c = s[i];
        int d = c >= L'0' && c <= L'9' ? c - L'0' : c >= L'A' && c <= L'F' ? c - L'A' + 10
              : c >= L'a' && c <= L'f' ? c - L'a' + 10 : -1;
        if (d < 0) return -1;
        v = v * 16 + d;
    }
    return v;
}

/* The device list row for the first hardware ID, "USB\VID_xxxx&PID_yyyy..." */
static const struct ozzy_device* lookup_model(PWDFDEVICE_INIT init) {
    WCHAR ids[256] = { 0 };
    ULONG length = 0;
    if (!NT_SUCCESS(WdfFdoInitQueryProperty(init, DevicePropertyHardwareID, sizeof(ids) - 2 * sizeof(WCHAR), ids,
                                            &length)))
        return NULL;
    static const WCHAR prefix[] = L"USB\\VID_";
    for (int i = 0; prefix[i]; i++) {
        if (RtlUpcaseUnicodeChar(ids[i]) != prefix[i]) return NULL;
    }
    /* USB\VID_ is 8 characters, then 4 digits, "&PID_" and 4 digits */
    int vid = hex4(ids + 8), pid = hex4(ids + 17);
    if (vid < 0 || pid < 0) return NULL;
    return ozzy_device_lookup((uint16_t)vid, (uint16_t)pid);
}

NTSTATUS wdev_evt_add(WDFDRIVER driver, PWDFDEVICE_INIT init) {
    UNREFERENCED_PARAMETER(driver);
    PAGED_CODE();

    const struct ozzy_device* model = lookup_model(init);
    if (!model) return STATUS_NOT_SUPPORTED;
    /* The Ploytec isochronous family and USB MIDI come later (isochronous URBs, a MIDI circuit) */
    if (model->family != OZZY_FAMILY_PLOYTEC_XONE) {
        DbgPrintEx(DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "[Ozzy] %s: not supported on Windows yet\n", model->name);
        return STATUS_NOT_SUPPORTED;
    }

    ACX_DEVICEINIT_CONFIG device_init_config;
    ACX_DEVICEINIT_CONFIG_INIT(&device_init_config);
    NTSTATUS status = AcxDeviceInitInitialize(init, &device_init_config);
    if (!NT_SUCCESS(status)) return status;

    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = wdev_evt_prepare_hardware;
    pnp.EvtDeviceReleaseHardware = wdev_evt_release_hardware;
    pnp.EvtDeviceD0Entry = wdev_evt_d0_entry;
    pnp.EvtDeviceD0Exit = wdev_evt_d0_exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, wdev);
    WDFDEVICE device;
    status = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;

    struct wdev* ctx = wdev_get(device);
    ctx->model = model;
    /* The engine runs at the highest rate the device lists (PloytecEngine's default) */
    uint32_t rates[OZZY_MAX_RATES];
    ctx->rate = rates[ozzy_device_rates(model, rates) - 1];

    ACX_DEVICE_CONFIG device_config;
    ACX_DEVICE_CONFIG_INIT(&device_config);
    status = AcxDeviceInitialize(device, &device_config);
    if (!NT_SUCCESS(status)) return status;

    WDF_DEVICE_PNP_CAPABILITIES caps;
    WDF_DEVICE_PNP_CAPABILITIES_INIT(&caps);
    caps.SurpriseRemovalOK = WdfTrue;
    WdfDeviceSetPnpCapabilities(device, &caps);

    /* Circuits exist from here; they become endpoints when added in PrepareHardware */
    if (model->out_channels) {
        status = wcircuit_create(device, true, &ctx->render);
        if (!NT_SUCCESS(status)) return status;
    }
    if (model->in_channels) {
        status = wcircuit_create(device, false, &ctx->capture);
        if (!NT_SUCCESS(status)) return status;
    }
    DbgPrintEx(DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "[Ozzy] %s (build %s), %u in / %u out at %u Hz\n",
               model->name, OZZY_GIT_VERSION, model->in_channels, model->out_channels, ctx->rate);
    return STATUS_SUCCESS;
}

/* The engine and its bus on the shared memory, configured once; then the circuits. Can run
 * again after ReleaseHardware (rebalance), so everything is created here, not in DeviceAdd. */
static NTSTATUS wdev_evt_prepare_hardware(WDFDEVICE device, WDFCMRESLIST resources, WDFCMRESLIST translated) {
    UNREFERENCED_PARAMETER(resources);
    UNREFERENCED_PARAMETER(translated);
    PAGED_CODE();
    struct wdev* ctx = wdev_get(device);

    if (!ctx->shm) {
        ctx->shm = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*ctx->shm), OZZY_POOL_TAG);
        if (!ctx->shm) return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (!ctx->engine) {
        size_t size = ozzy_engine_size(ctx->model);
        void* mem = size ? ExAllocatePool2(POOL_FLAG_NON_PAGED, size, OZZY_POOL_TAG) : NULL;
        ctx->engine = ozzy_engine_init(mem, ctx->model);
        if (!ctx->engine) {
            if (mem) ExFreePoolWithTag(mem, OZZY_POOL_TAG);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    if (!ctx->configured) {
        kbus_init(&ctx->bus, ctx->engine);
        NTSTATUS status = kbus_create(&ctx->bus, device);
        if (NT_SUCCESS(status)) status = kbus_configure(&ctx->bus, ctx->shm, sizeof(*ctx->shm));
        if (!NT_SUCCESS(status)) return status;
        ozzy_engine_attach(ctx->engine, &ctx->bus.bus, ctx->shm);
        ctx->configured = true;
    }

    if (!ctx->circuits_added) {
        if (ctx->render) {
            NTSTATUS status = AcxDeviceAddCircuit(device, ctx->render);
            if (!NT_SUCCESS(status)) return status;
        }
        if (ctx->capture) {
            NTSTATUS status = AcxDeviceAddCircuit(device, ctx->capture);
            if (!NT_SUCCESS(status)) return status;
        }
        ctx->circuits_added = true;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS wdev_evt_release_hardware(WDFDEVICE device, WDFCMRESLIST translated) {
    UNREFERENCED_PARAMETER(translated);
    PAGED_CODE();
    struct wdev* ctx = wdev_get(device);

    if (ctx->circuits_added) {
        if (ctx->render) AcxDeviceRemoveCircuit(device, ctx->render);
        if (ctx->capture) AcxDeviceRemoveCircuit(device, ctx->capture);
        ctx->circuits_added = false;
    }
    /* D0Exit has stopped the engine and its transfers */
    kbus_release(&ctx->bus);
    if (ctx->engine) ExFreePoolWithTag(ctx->engine, OZZY_POOL_TAG);
    ctx->engine = NULL;
    ctx->configured = false;
    if (ctx->shm) ExFreePoolWithTag(ctx->shm, OZZY_POOL_TAG);
    ctx->shm = NULL;
    return STATUS_SUCCESS;
}

/* Handshake and streaming, as the kext's start: the device streams (silence, MIDI) for as long
 * as it is in D0, and the ACX streams attach to it */
static NTSTATUS wdev_evt_d0_entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous) {
    UNREFERENCED_PARAMETER(previous);
    PAGED_CODE();
    struct wdev* ctx = wdev_get(device);

    kbus_start_io(&ctx->bus);
    if (!ozzy_engine_start(ctx->engine)) {
        ozzy_engine_stop(ctx->engine);
        kbus_stop_io(&ctx->bus);
        return STATUS_DEVICE_NOT_READY;
    }
    if (ctx->shm->audio.sample_rate != ctx->rate)
        DbgPrintEx(DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "[Ozzy] device runs at %u Hz, the format says %u Hz\n",
                   (uint32_t)ctx->shm->audio.sample_rate, ctx->rate);
    return STATUS_SUCCESS;
}

static NTSTATUS wdev_evt_d0_exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE target) {
    UNREFERENCED_PARAMETER(target);
    PAGED_CODE();
    struct wdev* ctx = wdev_get(device);

    /* No more resubmits, then cancel what is in flight */
    ozzy_engine_stop(ctx->engine);
    kbus_stop_io(&ctx->bus);
    return STATUS_SUCCESS;
}
