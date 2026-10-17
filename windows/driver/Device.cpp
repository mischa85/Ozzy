#include "Device.h"
#include "../../engine/devices/ploytec/PloytecEngine.h"
#include "../../common/ozzy_version.h"

static EVT_WDF_DEVICE_PREPARE_HARDWARE OzzyEvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE OzzyEvtReleaseHardware;
static EVT_WDF_DEVICE_D0_ENTRY OzzyEvtD0Entry;
static EVT_WDF_DEVICE_D0_EXIT OzzyEvtD0Exit;

/* Four hex digits at @s, or -1 */
static int Hex4(const WCHAR* s) {
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
static const struct ozzy_device* LookupModel(PWDFDEVICE_INIT init) {
    WCHAR ids[256] = {};
    ULONG length = 0;
    if (!NT_SUCCESS(WdfFdoInitQueryProperty(init, DevicePropertyHardwareID, sizeof(ids) - 2 * sizeof(WCHAR), ids,
                                            &length)))
        return nullptr;
    static const wchar_t prefix[] = L"USB\\VID_";
    for (int i = 0; prefix[i]; i++) {
        if (RtlUpcaseUnicodeChar(ids[i]) != prefix[i]) return nullptr;
    }
    // USB\VID_ is 8 characters, then 4 digits, "&PID_" and 4 digits
    int vid = Hex4(ids + 8), pid = Hex4(ids + 17);
    if (vid < 0 || pid < 0) return nullptr;
    return ozzy_device_lookup((uint16_t)vid, (uint16_t)pid);
}

NTSTATUS OzzyEvtDeviceAdd(WDFDRIVER driver, PWDFDEVICE_INIT init) {
    UNREFERENCED_PARAMETER(driver);
    PAGED_CODE();

    const struct ozzy_device* model = LookupModel(init);
    if (!model) return STATUS_NOT_SUPPORTED;
    // The Ploytec isochronous family and USB MIDI come later (isochronous URBs, a MIDI circuit)
    if (model->family != OZZY_FAMILY_PLOYTEC_XONE) {
        DbgPrintEx(DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "[Ozzy] %s: not supported on Windows yet\n", model->name);
        return STATUS_NOT_SUPPORTED;
    }

    ACX_DEVICEINIT_CONFIG deviceInitConfig;
    ACX_DEVICEINIT_CONFIG_INIT(&deviceInitConfig);
    NTSTATUS status = AcxDeviceInitInitialize(init, &deviceInitConfig);
    if (!NT_SUCCESS(status)) return status;

    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = OzzyEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = OzzyEvtReleaseHardware;
    pnp.EvtDeviceD0Entry = OzzyEvtD0Entry;
    pnp.EvtDeviceD0Exit = OzzyEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, OzzyDeviceContext);
    WDFDEVICE device;
    status = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;

    OzzyDeviceContext* ctx = GetOzzyDevice(device);
    ctx->model = model;
    // The engine runs at the highest rate the device lists (PloytecEngine's default)
    uint32_t rates[OZZY_MAX_RATES];
    ctx->rate = rates[ozzy_device_rates(model, rates) - 1];

    ACX_DEVICE_CONFIG deviceConfig;
    ACX_DEVICE_CONFIG_INIT(&deviceConfig);
    status = AcxDeviceInitialize(device, &deviceConfig);
    if (!NT_SUCCESS(status)) return status;

    WDF_DEVICE_PNP_CAPABILITIES caps;
    WDF_DEVICE_PNP_CAPABILITIES_INIT(&caps);
    caps.SurpriseRemovalOK = WdfTrue;
    WdfDeviceSetPnpCapabilities(device, &caps);

    // Circuits exist from here; they become endpoints when added in PrepareHardware
    if (model->out_channels) {
        status = OzzyCreateCircuit(device, true, &ctx->render);
        if (!NT_SUCCESS(status)) return status;
    }
    if (model->in_channels) {
        status = OzzyCreateCircuit(device, false, &ctx->capture);
        if (!NT_SUCCESS(status)) return status;
    }
    DbgPrintEx(DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "[Ozzy] %s (build %s), %u in / %u out at %u Hz\n",
               model->name, OZZY_GIT_VERSION, model->in_channels, model->out_channels, ctx->rate);
    return STATUS_SUCCESS;
}

/* The engine and its bus on the shared memory, configured once; then the circuits. Can run
 * again after ReleaseHardware (rebalance), so everything is created here, not in DeviceAdd. */
static NTSTATUS OzzyEvtPrepareHardware(WDFDEVICE device, WDFCMRESLIST resources, WDFCMRESLIST translated) {
    UNREFERENCED_PARAMETER(resources);
    UNREFERENCED_PARAMETER(translated);
    PAGED_CODE();
    OzzyDeviceContext* ctx = GetOzzyDevice(device);

    if (!ctx->shm) {
        ctx->shm = (ozzy_shared_memory*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(ozzy_shared_memory), OZZY_POOL_TAG);
        if (!ctx->shm) return STATUS_INSUFFICIENT_RESOURCES;
        ctx->shm->magic = OZZY_MAGIC;
        ctx->shm->version = OZZY_SHARED_VERSION;
        ctx->shm->vendor_id = ctx->model->vid;
        ctx->shm->product_id = ctx->model->pid;
        ozzy_strlcpy(ctx->shm->product_name, ctx->model->name, sizeof(ctx->shm->product_name));
    }
    if (!ctx->bus) {
        ctx->bus = new KmdfBus();
        ctx->engine = new PloytecEngine(ctx->model);
        if (!ctx->bus || !ctx->engine) return STATUS_INSUFFICIENT_RESOURCES;
        ctx->bus->SetEngine(ctx->engine);
    }
    if (!ctx->configured) {
        NTSTATUS status = ctx->bus->Create(device);
        if (NT_SUCCESS(status)) status = ctx->bus->Configure(ctx->engine, ctx->shm, sizeof(ozzy_shared_memory));
        if (!NT_SUCCESS(status)) return status;
        ctx->engine->Init(ctx->bus, ctx->shm);
        ctx->configured = true;
    }

    if (!ctx->circuitsAdded) {
        if (ctx->render) {
            NTSTATUS status = AcxDeviceAddCircuit(device, ctx->render);
            if (!NT_SUCCESS(status)) return status;
        }
        if (ctx->capture) {
            NTSTATUS status = AcxDeviceAddCircuit(device, ctx->capture);
            if (!NT_SUCCESS(status)) return status;
        }
        ctx->circuitsAdded = true;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS OzzyEvtReleaseHardware(WDFDEVICE device, WDFCMRESLIST translated) {
    UNREFERENCED_PARAMETER(translated);
    PAGED_CODE();
    OzzyDeviceContext* ctx = GetOzzyDevice(device);

    if (ctx->circuitsAdded) {
        if (ctx->render) AcxDeviceRemoveCircuit(device, ctx->render);
        if (ctx->capture) AcxDeviceRemoveCircuit(device, ctx->capture);
        ctx->circuitsAdded = false;
    }
    // D0Exit has stopped the engine and its transfers
    delete ctx->engine;
    delete ctx->bus;
    ctx->engine = nullptr;
    ctx->bus = nullptr;
    ctx->configured = false;
    if (ctx->shm) ExFreePoolWithTag(ctx->shm, OZZY_POOL_TAG);
    ctx->shm = nullptr;
    return STATUS_SUCCESS;
}

/* Handshake and streaming, as the kext's start: the device streams (silence, MIDI) for as long
 * as it is in D0, and the ACX streams attach to it */
static NTSTATUS OzzyEvtD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous) {
    UNREFERENCED_PARAMETER(previous);
    PAGED_CODE();
    OzzyDeviceContext* ctx = GetOzzyDevice(device);

    ctx->bus->StartIo();
    if (!ctx->engine->Start()) {
        ctx->engine->Stop();
        ctx->bus->StopIo();
        return STATUS_DEVICE_NOT_READY;
    }
    if (ctx->shm->audio.sample_rate != ctx->rate)
        DbgPrintEx(DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "[Ozzy] device runs at %u Hz, the format says %u Hz\n",
                   (uint32_t)ctx->shm->audio.sample_rate, ctx->rate);
    return STATUS_SUCCESS;
}

static NTSTATUS OzzyEvtD0Exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE target) {
    UNREFERENCED_PARAMETER(target);
    PAGED_CODE();
    OzzyDeviceContext* ctx = GetOzzyDevice(device);

    // No more resubmits, then cancel what is in flight
    ctx->engine->Stop();
    ctx->bus->StopIo();
    return STATUS_SUCCESS;
}
