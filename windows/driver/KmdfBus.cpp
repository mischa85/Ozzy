#include "KmdfBus.h"
#include "../../common/ozzy_ep.h"
#include <ntstrsafe.h>
#include <stdarg.h>

#define OZZY_POOL_TAG 'yzzO'

// --- struct ozzy_bus_ops: the engine's calls, to the methods ---
static bool OpsSubmit(void* ctx, uint8_t pipe, void* buf, uint32_t size, void* xfer) {
    return ((KmdfBus*)ctx)->SubmitUSBPacket(pipe, buf, size, xfer);
}
static bool OpsControl(void* ctx, uint8_t type, uint8_t req, uint16_t val, uint16_t idx, void* data, uint16_t len) {
    return ((KmdfBus*)ctx)->VendorRequest(type, req, val, idx, data, len);
}
static bool OpsStartTimer(void* ctx, uint32_t intervalUs) { return ((KmdfBus*)ctx)->StartTimer(intervalUs); }
static void OpsStopTimer(void* ctx) { ((KmdfBus*)ctx)->StopTimer(); }
static void OpsPcmProgress(void* ctx, enum ozzy_pcm_dir dir, uint64_t frames) { ((KmdfBus*)ctx)->PcmProgress(dir, frames); }
static void OpsSleep(void* ctx, unsigned int ms) { ((KmdfBus*)ctx)->Sleep(ms); }
static uint64_t OpsTime(void* ctx) { return ((KmdfBus*)ctx)->GetTime(); }

// Isochronous URBs (per-packet lengths, ring wrap) come with the Ploytec isochronous family
const struct ozzy_bus_ops KmdfBus::sOps = {
    OpsSubmit, nullptr, OpsControl, OpsStartTimer, OpsStopTimer, OpsPcmProgress, OpsSleep, OpsTime
};

KmdfBus::KmdfBus() {
    mOzzyBus.ops = &sOps;
    mOzzyBus.ctx = this;
}

/* ReleaseHardware: everything Create and Configure made, so PrepareHardware can run again */
KmdfBus::~KmdfBus() {
    StopTimer();
    for (int i = 0; i < mPipeCount; i++) {
        for (int k = 0; k < kRequestsPerPipe; k++) {
            if (mPipes[i].requests[k]) WdfObjectDelete(mPipes[i].requests[k]);
        }
    }
    if (mShm) WdfObjectDelete(mShm);
    if (mUsb) WdfObjectDelete(mUsb);
    if (mConfig) ExFreePoolWithTag(mConfig, OZZY_POOL_TAG);
}

NTSTATUS KmdfBus::Create(WDFDEVICE device) {
    mDevice = device;

    WDF_USB_DEVICE_CREATE_CONFIG config;
    WDF_USB_DEVICE_CREATE_CONFIG_INIT(&config, USBD_CLIENT_CONTRACT_VERSION_602);
    NTSTATUS status = WdfUsbTargetDeviceCreateWithParameters(device, &config, WDF_NO_OBJECT_ATTRIBUTES, &mUsb);
    if (!NT_SUCCESS(status)) {
        Log("[Ozzy] USB target: 0x%08x", status);
        return status;
    }

    // Configuration 1 with every interface at setting 0; Configure selects the profile's setting
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS params;
    if (WdfUsbTargetDeviceGetNumInterfaces(mUsb) == 1)
        WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_SINGLE_INTERFACE(&params);
    else
        WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_MULTIPLE_INTERFACES(&params, 0, nullptr);
    status = WdfUsbTargetDeviceSelectConfig(mUsb, WDF_NO_OBJECT_ATTRIBUTES, &params);
    if (!NT_SUCCESS(status)) {
        Log("[Ozzy] select configuration: 0x%08x", status);
        return status;
    }

    // The raw configuration descriptor: bmAttributes and wMaxPacketSize as the device has them
    USHORT size = 0;
    WdfUsbTargetDeviceRetrieveConfigDescriptor(mUsb, nullptr, &size);
    mConfig = (PUSB_CONFIGURATION_DESCRIPTOR)ExAllocatePool2(POOL_FLAG_NON_PAGED, size, OZZY_POOL_TAG);
    if (!mConfig) return STATUS_INSUFFICIENT_RESOURCES;
    return WdfUsbTargetDeviceRetrieveConfigDescriptor(mUsb, mConfig, &size);
}

/* Endpoint @address of interface @intf, alternate setting @alt, in the configuration descriptor */
const USB_ENDPOINT_DESCRIPTOR* KmdfBus::FindEndpoint(uint8_t intf, uint8_t alt, uint8_t address) const {
    const uint8_t* p = (const uint8_t*)mConfig;
    const uint8_t* end = p + mConfig->wTotalLength;
    bool inSetting = false;
    while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end) {
        if (p[1] == USB_INTERFACE_DESCRIPTOR_TYPE && p[0] >= sizeof(USB_INTERFACE_DESCRIPTOR)) {
            auto* d = (const USB_INTERFACE_DESCRIPTOR*)p;
            inSetting = d->bInterfaceNumber == intf && d->bAlternateSetting == alt;
        } else if (inSetting && p[1] == USB_ENDPOINT_DESCRIPTOR_TYPE && p[0] >= sizeof(USB_ENDPOINT_DESCRIPTOR)) {
            auto* e = (const USB_ENDPOINT_DESCRIPTOR*)p;
            if (e->bEndpointAddress == address) return e;
        }
        p += p[0];
    }
    return nullptr;
}

NTSTATUS KmdfBus::Configure(struct ozzy_engine* engine, void* shm, size_t shmSize) {
    const struct ozzy_device_profile& profile = engine->profile;

    mShmBase = (uint8_t*)shm;
    NTSTATUS status = WdfMemoryCreatePreallocated(WDF_NO_OBJECT_ATTRIBUTES, shm, shmSize, &mShm);
    if (!NT_SUCCESS(status)) return status;

    // The profile's interfaces, with its alternate setting
    WDFUSBINTERFACE byIndex[4] = {};
    UCHAR count = WdfUsbTargetDeviceGetNumInterfaces(mUsb);
    for (int i = 0; i < profile.interface_count; i++) {
        for (UCHAR k = 0; k < count; k++) {
            WDFUSBINTERFACE intf = WdfUsbTargetDeviceGetInterface(mUsb, k);
            if (WdfUsbInterfaceGetInterfaceNumber(intf) == profile.interfaces[i]) byIndex[i] = intf;
        }
        if (!byIndex[i]) {
            Log("[Ozzy] interface %u not found", profile.interfaces[i]);
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        if (profile.alt_setting) {
            WDF_USB_INTERFACE_SELECT_SETTING_PARAMS params;
            WDF_USB_INTERFACE_SELECT_SETTING_PARAMS_INIT_SETTING(&params, profile.alt_setting);
            status = WdfUsbInterfaceSelectSetting(byIndex[i], WDF_NO_OBJECT_ATTRIBUTES, &params);
            if (!NT_SUCCESS(status)) {
                Log("[Ozzy] interface %u: alternate setting %u: 0x%08x", profile.interfaces[i],
                    profile.alt_setting, status);
                return status;
            }
        }
    }

    // Each role: a fixed address, or the first endpoint of the wanted type (as OzzyKext)
    for (int role = 0; role < OZZY_PIPE_ROLES; role++) {
        const struct ozzy_ep* cfg = &profile.pipes[role];
        if (cfg->address == 0 && cfg->types == 0) continue;

        WDFUSBINTERFACE intf = nullptr;
        for (int i = 0; i < profile.interface_count; i++) {
            if (profile.interfaces[i] == cfg->intf) intf = byIndex[i];
        }
        if (!intf) continue;
        uint8_t alt = WdfUsbInterfaceGetConfiguredSettingIndex(intf);

        bool found = false;
        BYTE pipes = WdfUsbInterfaceGetNumConfiguredPipes(intf);
        for (BYTE j = 0; j < pipes && !found; j++) {
            WDF_USB_PIPE_INFORMATION info;
            WDF_USB_PIPE_INFORMATION_INIT(&info);
            WDFUSBPIPE pipe = WdfUsbInterfaceGetConfiguredPipe(intf, j, &info);
            const USB_ENDPOINT_DESCRIPTOR* e = FindEndpoint(cfg->intf, alt, info.EndpointAddress);
            if (!pipe || !e) continue;
            uint8_t type = e->bmAttributes & 3;
            if (cfg->address ? e->bEndpointAddress != cfg->address
                             : !ozzy_ep_matches(cfg, e->bEndpointAddress, e->bmAttributes)) continue;
            found = true;

            status = AddPipe(pipe, e->bEndpointAddress);
            if (!NT_SUCCESS(status)) return status;
            // Bytes per service interval: wMaxPacketSize times the High Speed high-bandwidth multiplier
            uint16_t w = e->wMaxPacketSize;
            uint16_t maxPacketSize = (uint16_t)((w & 0x07FF) * (((w >> 11) & 3) + 1));
            Log("[Ozzy]   pipe 0x%02x (%s, wMaxPacketSize %u, bInterval %u)", e->bEndpointAddress,
                type == 1 ? "isochronous" : type == 2 ? "bulk" : "interrupt", maxPacketSize, e->bInterval);
            ozzy_engine_set_endpoint(engine, (enum ozzy_pipe_role)role, e->bEndpointAddress, type, maxPacketSize, e->bInterval);
        }
        if (!found) Log("[Ozzy]   no endpoint for pipe role %d on interface %u", role, cfg->intf);
    }
    return STATUS_SUCCESS;
}

/* The pipe's requests, created once and reused for every transfer */
NTSTATUS KmdfBus::AddPipe(WDFUSBPIPE pipe, uint8_t address) {
    if (mPipeCount == kMaxPipes) return STATUS_INSUFFICIENT_RESOURCES;
    Pipe& p = mPipes[mPipeCount];
    p.pipe = pipe;
    p.address = address;
    p.busy = 0;
    if (address & 0x80) WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(pipe);

    for (int i = 0; i < kRequestsPerPipe; i++) {
        WDF_OBJECT_ATTRIBUTES attributes;
        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
        attributes.ParentObject = mDevice;
        NTSTATUS status = WdfRequestCreate(&attributes, WdfUsbTargetPipeGetIoTarget(pipe), &p.requests[i]);
        if (!NT_SUCCESS(status)) return status;
        p.slots[i] = { this, (uint8_t)mPipeCount, nullptr };
    }
    mPipeCount++;
    return STATUS_SUCCESS;
}

KmdfBus::Pipe* KmdfBus::FindPipe(uint8_t address) {
    for (int i = 0; i < mPipeCount; i++) {
        if (mPipes[i].address == address) return &mPipes[i];
    }
    return nullptr;
}

bool KmdfBus::SubmitUSBPacket(uint8_t pipeID, void* buffer, uint32_t size, void* contextID) {
    Pipe* p = FindPipe(pipeID);
    if (!p || !mShm) return false;

    // A free request: claim its bit
    int i;
    for (;;) {
        LONG busy = p->busy;
        for (i = 0; i < kRequestsPerPipe && (busy & (1 << i)); i++) {}
        if (i == kRequestsPerPipe) return false;
        if (InterlockedCompareExchange(&p->busy, busy | (1 << i), busy) == busy) break;
    }
    WDFREQUEST request = p->requests[i];

    WDF_REQUEST_REUSE_PARAMS reuse;
    WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
    WdfRequestReuse(request, &reuse);

    // Zero copy: the request points into the shared memory
    WDFMEMORY_OFFSET offset = { (size_t)((uint8_t*)buffer - mShmBase), size };
    NTSTATUS status = (pipeID & 0x80) ? WdfUsbTargetPipeFormatRequestForRead(p->pipe, request, mShm, &offset)
                                      : WdfUsbTargetPipeFormatRequestForWrite(p->pipe, request, mShm, &offset);
    if (NT_SUCCESS(status)) {
        p->slots[i].context = contextID;
        WdfRequestSetCompletionRoutine(request, Completion, &p->slots[i]);
        if (WdfRequestSend(request, WdfUsbTargetPipeGetIoTarget(p->pipe), WDF_NO_SEND_OPTIONS)) return true;
        status = WdfRequestGetStatus(request);
    }
    InterlockedAnd(&p->busy, ~(1 << i));
    if (status != STATUS_CANCELLED) Log("[Ozzy] pipe 0x%02x submit failed: 0x%08x", pipeID, status);
    return false;
}

/* DISPATCH_LEVEL: release the request, then hand the result to the engine, which resubmits and
 * moves PCM through its rings from here */
void KmdfBus::Completion(WDFREQUEST request, WDFIOTARGET target, PWDF_REQUEST_COMPLETION_PARAMS params,
                         WDFCONTEXT context) {
    UNREFERENCED_PARAMETER(target);
    Slot* slot = (Slot*)context;
    KmdfBus* bus = slot->bus;
    Pipe& p = bus->mPipes[slot->pipe];
    void* engineContext = slot->context;
    NTSTATUS status = params->IoStatus.Status;
    ULONG bytes = (p.address & 0x80) ? params->Parameters.Usb.Completion->Parameters.PipeRead.Length
                                     : params->Parameters.Usb.Completion->Parameters.PipeWrite.Length;

    int i = (int)(slot - p.slots);
    UNREFERENCED_PARAMETER(request);
    InterlockedAnd(&p.busy, ~(1 << i));

    InterlockedIncrement(&bus->mInCompletion);
    if (!NT_SUCCESS(status) && status != STATUS_CANCELLED)
        DbgPrintEx(DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "[Ozzy] pipe 0x%02x error: 0x%08x\n", p.address, status);
    if (bus->mEngine) ozzy_engine_complete(bus->mEngine, p.address, engineContext, (int)status, bytes);
    InterlockedDecrement(&bus->mInCompletion);
}

void KmdfBus::StopIo() {
    for (int i = 0; i < mPipeCount; i++)
        WdfIoTargetStop(WdfUsbTargetPipeGetIoTarget(mPipes[i].pipe), WdfIoTargetCancelSentIo);
}

void KmdfBus::StartIo() {
    for (int i = 0; i < mPipeCount; i++) WdfIoTargetStart(WdfUsbTargetPipeGetIoTarget(mPipes[i].pipe));
}

void KmdfBus::SetPcmClient(enum ozzy_pcm_dir dir, IOzzyPcmClient* client) {
    InterlockedExchangePointer((PVOID volatile*)&mClient[dir], client);
}

/* PASSIVE_LEVEL. A completion that started before a detach may still use what was detached;
 * once none is in progress, every later one sees the detach. */
void KmdfBus::WaitCompletions() {
    for (int i = 0; i < 200 && InterlockedCompareExchange(&mInCompletion, 0, 0) != 0; i++) Sleep(1);
    KeFlushQueuedDpcs();
}

void KmdfBus::PcmProgress(enum ozzy_pcm_dir dir, uint64_t frames) {
    IOzzyPcmClient* client = (IOzzyPcmClient*)InterlockedCompareExchangePointer((PVOID volatile*)&mClient[dir],
                                                                               nullptr, nullptr);
    if (client) client->OnPcmProgress(frames, GetTime());
}

/* PASSIVE_LEVEL (the handshake), 5 s timeout like the kext's deviceRequest */
bool KmdfBus::VendorRequest(uint8_t type, uint8_t req, uint16_t val, uint16_t idx, void* data, uint16_t len) {
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
    NTSTATUS status = WdfUsbTargetDeviceSendControlTransferSynchronously(mUsb, WDF_NO_HANDLE, &options, &setup,
                                                                        len ? &memory : nullptr, &done);
    if (NT_SUCCESS(status)) return true;
    Log("[Ozzy] control request 0x%02x (type 0x%02x, wValue 0x%04x) failed: 0x%08x", req, type, val, status);
    return false;
}

void KmdfBus::TimerCallback(PEX_TIMER timer, PVOID context) {
    UNREFERENCED_PARAMETER(timer);
    KmdfBus* bus = (KmdfBus*)context;
    if (bus->mEngine) ozzy_engine_timer(bus->mEngine);
}

bool KmdfBus::StartTimer(uint32_t intervalUs) {
    if (!mTimer) mTimer = ExAllocateTimer(TimerCallback, this, EX_TIMER_HIGH_RESOLUTION);
    if (!mTimer) return false;
    LONGLONG period = (LONGLONG)intervalUs * 10;   // 100 ns units
    ExSetTimer(mTimer, -period, period, nullptr);
    return true;
}

void KmdfBus::StopTimer() {
    if (!mTimer) return;
    // Waits for a callback in progress
    ExDeleteTimer(mTimer, TRUE, TRUE, nullptr);
    mTimer = nullptr;
}

void KmdfBus::Sleep(uint32_t ms) {
    LARGE_INTEGER delay;
    delay.QuadPart = -(LONGLONG)ms * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &delay);
}

uint64_t KmdfBus::GetTime() {
    return (uint64_t)KeQueryPerformanceCounter(nullptr).QuadPart;
}

void KmdfBus::Log(const char* fmt, ...) {
    char line[256];
    va_list args;
    va_start(args, fmt);
    RtlStringCbVPrintfA(line, sizeof(line), fmt, args);
    va_end(args);
    DbgPrintEx(DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "%s\n", line);
}
