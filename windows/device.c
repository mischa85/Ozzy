#include "device.h"
#include "../common/ozzy_version.h"

static EVT_WDF_DEVICE_PREPARE_HARDWARE wdev_evt_prepare_hardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE wdev_evt_release_hardware;
static EVT_WDF_DEVICE_D0_ENTRY wdev_evt_d0_entry;
static EVT_WDF_DEVICE_D0_EXIT wdev_evt_d0_exit;

/* The device list row for the first hardware ID (ozzy_device_lookup_hwid) */
static const struct ozzy_device* lookup_model(PWDFDEVICE_INIT init) {
    WCHAR ids[256] = { 0 };
    ULONG length = 0;
    if (!NT_SUCCESS(WdfFdoInitQueryProperty(init, DevicePropertyHardwareID, sizeof(ids) - 2 * sizeof(WCHAR), ids,
                                            &length)))
        return NULL;
    return ozzy_device_lookup_hwid((const uint16_t*)ids);
}

NTSTATUS wdev_evt_add(WDFDRIVER driver, PWDFDEVICE_INIT init) {
    UNREFERENCED_PARAMETER(driver);
    PAGED_CODE();

    const struct ozzy_device* model = lookup_model(init);
    if (!model) return STATUS_NOT_SUPPORTED;
    /* USB MIDI (the Xone:1D) needs a MIDI port, which comes later */
    if (model->family == OZZY_FAMILY_USB_MIDI) {
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
    status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &ctx->lock);
    if (!NT_SUCCESS(status)) return status;

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
    DbgPrintEx(DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "[Ozzy] %s (build %s), %u in / %u out\n",
               model->name, OZZY_GIT_VERSION, model->in_channels, model->out_channels);
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
        if (NT_SUCCESS(status)) status = kbus_configure(&ctx->bus);
        if (!NT_SUCCESS(status)) return status;
        ozzy_engine_attach(ctx->engine, &ctx->bus.bus, ctx->shm);
        if (!ozzy_engine_alloc_memory(ctx->engine, &ctx->bus.allocator)) return STATUS_INSUFFICIENT_RESOURCES;
        wsettings_restore(ctx, device);
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
    if (ctx->engine) ozzy_engine_free_memory(ctx->engine);
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

    WdfWaitLockAcquire(ctx->lock, NULL);
    kbus_start_io(&ctx->bus);
    ctx->started = ozzy_engine_start(ctx->engine);
    if (!ctx->started) {
        ozzy_engine_stop(ctx->engine);
        kbus_stop_io(&ctx->bus);
    }
    WdfWaitLockRelease(ctx->lock);
    return ctx->started ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
}

static NTSTATUS wdev_evt_d0_exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE target) {
    UNREFERENCED_PARAMETER(target);
    PAGED_CODE();
    struct wdev* ctx = wdev_get(device);

    /* No more resubmits, cancel what is in flight, and refuse new requests until D0 */
    WdfWaitLockAcquire(ctx->lock, NULL);
    ctx->started = false;
    ozzy_engine_stop(ctx->engine);
    kbus_stop_io(&ctx->bus);
    WdfWaitLockRelease(ctx->lock);
    return STATUS_SUCCESS;
}

NTSTATUS wdev_stream_rate(struct wdev* dev, bool render, uint32_t rate) {
    PAGED_CODE();
    NTSTATUS status = STATUS_SUCCESS;

    WdfWaitLockAcquire(dev->lock, NULL);
    if (!dev->engine || !dev->shm) {
        status = STATUS_DEVICE_NOT_READY;
    } else if (dev->shm->audio.sample_rate != rate && dev->streams[render ? OZZY_PCM_IN : OZZY_PCM_OUT]) {
        status = STATUS_DEVICE_BUSY;
    } else {
        enum ozzy_setting_change change = ozzy_engine_change_rate(dev->engine, rate);
        if (change == OZZY_SETTING_INVALID) status = STATUS_NOT_SUPPORTED;
        else if (change == OZZY_SETTING_RESTART && dev->started && !(dev->started = ozzy_engine_restart(dev->engine)))
            status = STATUS_DEVICE_NOT_READY;
    }
    WdfWaitLockRelease(dev->lock);
    return status;
}
