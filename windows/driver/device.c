#include "device.h"
#include "midi.h"
#include "../../common/ozzy_version.h"

static EVT_WDF_DEVICE_PREPARE_HARDWARE wdev_evt_prepare_hardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE wdev_evt_release_hardware;
static EVT_WDF_DEVICE_D0_ENTRY wdev_evt_d0_entry;
static EVT_WDF_DEVICE_D0_EXIT wdev_evt_d0_exit;
static EVT_WDF_WORKITEM wdev_recover;
static EVT_WDF_TIMER wdev_stats_timer;

/* DISPATCH_LEVEL (the engine, through kbus): a stats incident. The clients hear of it 250 ms
 * on, once for a burst of them */
static void wdev_bus_stats(void* ctx) {
    struct wdev* dev = ctx;
    if (!InterlockedExchange(&dev->stats_pending, 1)) WdfTimerStart(dev->stats_timer, WDF_REL_TIMEOUT_IN_MS(250));
}

/* PASSIVE_LEVEL: the change event, which has them read the status and the stats again */
static void wdev_stats_timer(WDFTIMER timer) {
    struct wdev* dev = wdev_get(WdfTimerGetParentObject(timer));
    InterlockedExchange(&dev->stats_pending, 0);
    wsettings_changed(dev);
}

/* DISPATCH_LEVEL (kbus): a PCM pipe died */
static void wdev_bus_dead(void* ctx) {
    WdfWorkItemEnqueue(((struct wdev*)ctx)->recover);
}

/* A wedged pipe (the DB4's IN side after a long gap in completions: only a reset of the device
 * brings it back): the engine stopped, the port reset (the USB stack keeps the configuration and
 * the pipes), then a new start with its handshake. The streams stay; they miss what came between. */
static void wdev_recover(WDFWORKITEM item) {
    struct wdev* ctx = wdev_get(WdfWorkItemGetParentObject(item));
    WdfWaitLockAcquire(ctx->lock, NULL);
    if (ctx->started) {
        ozzy_engine_suspend(ctx->engine);
        kbus_stop_io(&ctx->bus);
        NTSTATUS status = STATUS_UNSUCCESSFUL;
        for (int k = 0; k < 3 && !NT_SUCCESS(status); k++) {
            if (k) {
                LARGE_INTEGER delay;
                delay.QuadPart = -100 * 10000;   /* 100 ms */
                KeDelayExecutionThread(KernelMode, FALSE, &delay);
            }
            status = WdfUsbTargetDeviceResetPortSynchronously(ctx->bus.usb);
            kbus_log("[Ozzy] port reset %d: 0x%08x", k + 1, status);
        }
        kbus_start_io(&ctx->bus);
        ctx->started = ozzy_engine_start(ctx->engine);
        if (!ctx->started) {
            ozzy_engine_stop(ctx->engine);
            kbus_stop_io(&ctx->bus);
        }
        kbus_log("[Ozzy] recovered: %s", ctx->started ? "streaming" : "start failed");
        ozzy_engine_stats_recovery(ctx->engine, ctx->started);
    }
    InterlockedExchange(&ctx->bus.recovering, 0);
    WdfWaitLockRelease(ctx->lock);
    wsettings_changed(ctx);
}

/* The device list row for the first hardware ID (ozzy_device_lookup_hwid) */
static const struct ozzy_device* lookup_model(PWDFDEVICE_INIT init) {
    WCHAR ids[256] = { 0 };
    ULONG length = 0;
    if (!NT_SUCCESS(WdfFdoInitQueryProperty(init, DevicePropertyHardwareID, sizeof(ids) - 2 * sizeof(WCHAR), ids,
                                            &length)))
        return NULL;
    return ozzy_device_lookup_hwid((const uint16_t*)ids);
}

/* devpkey.h's, here so no initguid.h is needed */
static const DEVPROPKEY wdev_key_friendly_name = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };
static const DEVPROPKEY wdev_key_bus_reported_desc = {
    { 0x540b947e, 0x8b40, 0x45bc, { 0xa8, 0xa2, 0x6a, 0x0b, 0x89, 0x4c, 0xbd, 0xa2 } }, 4 };

/* The device's own name (its USB product string, "Allen&Heath Xone:DB4") as its friendly name,
 * the one Windows shows and puts in its endpoints' names, as Linux and macOS name it; the INF
 * names it by the model ("Xone:DB4"), which stays if the device has no name of its own */
static void wdev_name(WDFDEVICE device) {
    WCHAR* name = wdev_get(device)->name;
    ULONG length = 0;
    DEVPROPTYPE type = DEVPROP_TYPE_EMPTY;
    WDF_DEVICE_PROPERTY_DATA data;

    WDF_DEVICE_PROPERTY_DATA_INIT(&data, &wdev_key_bus_reported_desc);
    if (!NT_SUCCESS(WdfDeviceQueryPropertyEx(device, &data, sizeof(wdev_get(device)->name) - sizeof(WCHAR), name,
                                             &length, &type)) ||
        type != DEVPROP_TYPE_STRING || length <= sizeof(WCHAR)) {
        name[0] = 0;
        return;
    }
    WDF_DEVICE_PROPERTY_DATA_INIT(&data, &wdev_key_friendly_name);
    data.Flags = PLUGPLAY_PROPERTY_PERSISTENT;
    NTSTATUS status = WdfDeviceAssignProperty(device, &data, DEVPROP_TYPE_STRING, length, name);
    if (!NT_SUCCESS(status)) kbus_log("[Ozzy] the device's name as its friendly name: 0x%08x", status);
}

/* The KS categories the INF registers the circuits under, as ksmedia.h has them (here so no
 * initguid.h is needed): AUDIO, RENDER, CAPTURE, REALTIME */
static const GUID wdev_ks_categories[] = {
    { 0x6994ad04, 0x93ef, 0x11d0, { 0xa3, 0xcc, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96 } },
    { 0x65e8773e, 0x8f56, 0x11d0, { 0xa3, 0xb9, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96 } },
    { 0x65e8773d, 0x8f56, 0x11d0, { 0xa3, 0xb9, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96 } },
    { 0xeb115ffc, 0x10c8, 0x4964, { 0x83, 0x1d, 0x6d, 0xcb, 0x02, 0xe6, 0xf2, 0x3f } },
};

/*
 * The same name on the circuits' KS filters: the FriendlyName of their interfaces (the INF sets
 * "Ozzy output", "Ozzy input", "Ozzy MIDI"; the reference strings are the circuit names, Ozzy*),
 * for clients that list the filters themselves (PortAudio's WDM-KS reads KSCATEGORY_AUDIO's).
 * After the circuits are added, which registers the interfaces.
 */
static void wdev_name_filters(struct wdev* ctx, WDFDEVICE device) {
    if (!ctx->name[0]) return;
    for (int c = 0; c < ARRAYSIZE(wdev_ks_categories); c++) {
        PWSTR links = NULL;
        /* Registered, not yet enabled: the device is still starting */
        NTSTATUS status = IoGetDeviceInterfaces(&wdev_ks_categories[c], WdfDeviceWdmGetPhysicalDevice(device),
                                                DEVICE_INTERFACE_INCLUDE_NONACTIVE, &links);
        if (!NT_SUCCESS(status) || !links) continue;
        for (PWSTR link = links; *link; link += wcslen(link) + 1) {
            PWSTR ref = wcsrchr(link, L'\\');
            if (!ref || _wcsnicmp(ref, L"\\Ozzy", 5)) continue;
            UNICODE_STRING name;
            HANDLE key = NULL;
            RtlInitUnicodeString(&name, link);
            status = IoOpenDeviceInterfaceRegistryKey(&name, KEY_SET_VALUE, &key);
            if (NT_SUCCESS(status)) {
                UNICODE_STRING value = RTL_CONSTANT_STRING(L"FriendlyName");
                status = ZwSetValueKey(key, &value, 0, REG_SZ, ctx->name,
                                       (ULONG)((wcslen(ctx->name) + 1) * sizeof(WCHAR)));
                ZwClose(key);
            }
            if (!NT_SUCCESS(status)) kbus_log("[Ozzy] the name of filter %ws: 0x%08x", ref + 1, status);
        }
        ExFreePool(links);
    }
}

NTSTATUS wdev_evt_add(WDFDRIVER driver, PWDFDEVICE_INIT init) {
    UNREFERENCED_PARAMETER(driver);
    PAGED_CODE();

    const struct ozzy_device* model = lookup_model(init);
    if (!model) return STATUS_NOT_SUPPORTED;

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
    wdev_name(device);
    status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &ctx->lock);
    if (!NT_SUCCESS(status)) return status;
    WDF_WORKITEM_CONFIG work;
    WDF_WORKITEM_CONFIG_INIT(&work, wdev_recover);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfWorkItemCreate(&work, &attributes, &ctx->recover);
    if (!NT_SUCCESS(status)) return status;
    WDF_TIMER_CONFIG timer;
    WDF_TIMER_CONFIG_INIT(&timer, wdev_stats_timer);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfTimerCreate(&timer, &attributes, &ctx->stats_timer);
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
    /* MIDI: every Ploytec device has it (in the PCM packets or on its own endpoints), and it is
     * all a USB MIDI device (the Xone:1D, no audio circuits) has. Audio works without it. */
    status = wmidi_circuit_create(device, &ctx->midi);
    if (!NT_SUCCESS(status)) {
        DbgPrintEx(DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "[Ozzy] no MIDI circuit: 0x%08x\n", status);
        ctx->midi = NULL;
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
        ctx->bus.service = wmidi_service;
        ctx->bus.service_ctx = ctx;
        ctx->bus.dead = wdev_bus_dead;
        ctx->bus.dead_ctx = ctx;
        ctx->bus.stats_changed = wdev_bus_stats;
        ctx->bus.stats_ctx = ctx;
        NTSTATUS status = kbus_create(&ctx->bus, device);
        if (NT_SUCCESS(status)) status = kbus_configure(&ctx->bus);
        if (!NT_SUCCESS(status)) return status;
        ozzy_engine_attach(ctx->engine, &ctx->bus.bus, ctx->shm);
        /* The connection and the build, for ozzy_info (the tools show them) */
        WDF_USB_DEVICE_INFORMATION usb_info;
        WDF_USB_DEVICE_INFORMATION_INIT(&usb_info);
        if (NT_SUCCESS(WdfUsbTargetDeviceRetrieveInformation(ctx->bus.usb, &usb_info)))
            ozzy_engine_set_usb_speed(ctx->engine, (usb_info.Traits & WDF_USB_DEVICE_TRAIT_AT_HIGH_SPEED) ?
                                      OZZY_USB_SPEED_HIGH : OZZY_USB_SPEED_FULL);
        ozzy_engine_set_driver_version(ctx->engine, OZZY_GIT_VERSION);
        /* No clock anchors: WaveRT reads the position each transfer moves (wstream_progress) */
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
        /* ACX takes the MIDI circuit next to the WaveRT ones; if it fails, audio goes on without */
        if (ctx->midi) {
            NTSTATUS status = AcxDeviceAddCircuit(device, ctx->midi);
            ctx->midi_added = NT_SUCCESS(status);
            if (!ctx->midi_added)
                DbgPrintEx(DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "[Ozzy] MIDI circuit not added: 0x%08x\n", status);
        }
        ctx->circuits_added = true;
        wdev_name_filters(ctx, device);
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
        if (ctx->midi_added) AcxDeviceRemoveCircuit(device, ctx->midi);
        ctx->midi_added = false;
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
    if (ctx->started) wsettings_changed(ctx);
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
    bool restarted = false;

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
        restarted = change == OZZY_SETTING_RESTART;   /* failed too: clients see the device stopped */
    }
    WdfWaitLockRelease(dev->lock);
    if (restarted) wsettings_changed(dev);
    return status;
}
