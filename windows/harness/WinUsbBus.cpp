#include "WinUsbBus.h"
#include "../../common/ozzy_ep.h"
#include <avrt.h>
#include <stdarg.h>
#include <stdio.h>

static const ULONG_PTR kQuitKey = 1;   // completion key of the packet that stops the thread

/* Run the calling thread as a real-time audio thread (MMCSS "Pro Audio", time critical) */
static void MakeAudioThread() {
    DWORD task = 0;
    AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}

WinUsbBus::~WinUsbBus() {
    Close();
}

bool WinUsbBus::Open(const wchar_t* path) {
    mFile = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (mFile == INVALID_HANDLE_VALUE) {
        Log("[WinUsbBus] opening the device failed: %lu", GetLastError());
        return false;
    }
    if (!WinUsb_Initialize(mFile, &mIntf[0])) {
        Log("[WinUsbBus] WinUsb_Initialize failed: %lu", GetLastError());
        return false;
    }

    // Every transfer completes through this port, on one thread
    mPort = CreateIoCompletionPort(mFile, nullptr, 0, 1);
    if (!mPort) {
        Log("[WinUsbBus] no completion port: %lu", GetLastError());
        return false;
    }

    ULONG len = 0;
    if (!WinUsb_GetDescriptor(mIntf[0], USB_DEVICE_DESCRIPTOR_TYPE, 0, 0, (PUCHAR)&mDeviceDesc,
                              sizeof(mDeviceDesc), &len)) {
        Log("[WinUsbBus] no device descriptor: %lu", GetLastError());
        return false;
    }

    // The first interface comes from WinUsb_Initialize, the others are "associated" with it
    USB_INTERFACE_DESCRIPTOR d;
    if (WinUsb_QueryInterfaceSettings(mIntf[0], 0, &d)) mIntfNumber[0] = d.bInterfaceNumber;
    mIntfCount = 1;
    for (UCHAR i = 0; mIntfCount < kMaxInterfaces; i++) {
        WINUSB_INTERFACE_HANDLE h;
        if (!WinUsb_GetAssociatedInterface(mIntf[0], i, &h)) break;   // ERROR_NO_MORE_ITEMS
        mIntf[mIntfCount] = h;
        mIntfNumber[mIntfCount] = WinUsb_QueryInterfaceSettings(h, 0, &d) ? d.bInterfaceNumber : 0xFF;
        mIntfCount++;
    }

    mCompletion = std::thread(&WinUsbBus::CompletionThread, this);
    return true;
}

void WinUsbBus::Close() {
    StopTimer();

    // Abort what is in flight; the engine has stopped resubmitting (OzzyEngine::Stop first)
    for (int s = 0; s < kPipeSlots; s++) {
        if (mPipeIntf[s]) WinUsb_AbortPipe(mPipeIntf[s], mPipeAddr[s]);
    }
    for (int i = 0; i < 2000 && mPending.load() > 0; i++) ::Sleep(1);
    if (mPending.load() > 0) Log("[WinUsbBus] %d transfers still pending at close", mPending.load());

    if (mCompletion.joinable()) {
        PostQueuedCompletionStatus(mPort, 0, kQuitKey, nullptr);
        mCompletion.join();
    }
    for (int s = 0; s < kPipeSlots; s++) mPipeIntf[s] = nullptr;
    for (int i = mIntfCount - 1; i >= 0; i--) {
        if (mIntf[i]) WinUsb_Free(mIntf[i]);
        mIntf[i] = nullptr;
    }
    mIntfCount = 0;
    if (mPort) { CloseHandle(mPort); mPort = nullptr; }
    if (mFile != INVALID_HANDLE_VALUE) { CloseHandle(mFile); mFile = INVALID_HANDLE_VALUE; }
}

bool WinUsbBus::Configure(OzzyEngine* engine) {
    const OzzyDeviceProfile& profile = engine->GetProfile();

    // The profile's interfaces, with its alternate setting
    WINUSB_INTERFACE_HANDLE byIndex[4] = {};
    for (int i = 0; i < profile.interfaceCount; i++) {
        for (int k = 0; k < mIntfCount; k++) {
            if (mIntfNumber[k] == profile.interfaceIndices[i]) byIndex[i] = mIntf[k];
        }
        if (!byIndex[i]) {
            Log("[WinUsbBus] interface %u not found (is WinUSB bound to the whole device, not one interface?)",
                profile.interfaceIndices[i]);
            return false;
        }
        if (profile.altSetting) {
            if (!WinUsb_SetCurrentAlternateSetting(byIndex[i], profile.altSetting)) {
                Log("[WinUsbBus] interface %u: alternate setting %u failed: %lu",
                    profile.interfaceIndices[i], profile.altSetting, GetLastError());
                return false;
            }
            ::Sleep(50);
        }
    }

    const OzzyPipeConfig* configs[kOzzyPipeRoles] = {
        &profile.pcmOut, &profile.pcmIn, &profile.midiIn, &profile.midiOut, &profile.feedback
    };
    for (int role = 0; role < kOzzyPipeRoles; role++) {
        const OzzyPipeConfig* cfg = configs[role];
        if (cfg->address == 0 && cfg->types == 0) continue;

        WINUSB_INTERFACE_HANDLE h = nullptr;
        for (int i = 0; i < profile.interfaceCount; i++) {
            if (profile.interfaceIndices[i] == cfg->intf) h = byIndex[i];
        }
        if (!h) {
            Log("[WinUsbBus] no interface %u for pipe role %d", cfg->intf, role);
            continue;
        }

        // The current setting's endpoints: a fixed address, or the first of the wanted type
        UCHAR alt = 0;
        USB_INTERFACE_DESCRIPTOR d;
        WinUsb_GetCurrentAlternateSetting(h, &alt);
        if (!WinUsb_QueryInterfaceSettings(h, alt, &d)) continue;
        bool found = false;
        for (UCHAR e = 0; e < d.bNumEndpoints && !found; e++) {
            WINUSB_PIPE_INFORMATION_EX pipe;
            if (!WinUsb_QueryPipeEx(h, alt, e, &pipe)) continue;
            uint8_t type = (uint8_t)pipe.PipeType;   // USBD_PIPE_TYPE is bmAttributes & 3
            if (cfg->address ? pipe.PipeId != cfg->address : !ozzy_ep_matches(cfg, pipe.PipeId, type)) continue;
            found = true;

            int slot = PipeSlot(pipe.PipeId);
            mPipeIntf[slot] = h;
            mPipeAddr[slot] = pipe.PipeId;
            // Bytes per service interval: wMaxPacketSize times the High Speed high-bandwidth multiplier
            uint16_t w = pipe.MaximumPacketSize;
            uint16_t maxPacketSize = (uint16_t)((w & 0x07FF) * (((w >> 11) & 3) + 1));
            Log("[WinUsbBus]   pipe 0x%02x opened (%s, wMaxPacketSize %u, bInterval %u)", pipe.PipeId,
                type == 1 ? "isochronous" : type == 2 ? "bulk" : "interrupt", maxPacketSize, pipe.Interval);
            engine->SetEndpoint((OzzyPipeRole)role, pipe.PipeId, type, maxPacketSize, pipe.Interval);
        }
        if (!found) Log("[WinUsbBus]   no endpoint for pipe role %d on interface %u", role, cfg->intf);
    }
    return true;
}

bool WinUsbBus::GetString(uint8_t index, char* dst, size_t size) {
    UCHAR buf[256];
    ULONG len = 0;
    if (!size) return false;
    dst[0] = 0;
    if (!index || !WinUsb_GetDescriptor(mIntf[0], USB_STRING_DESCRIPTOR_TYPE, index, 0x0409, buf, sizeof(buf), &len) ||
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

WinUsbBus::Stats WinUsbBus::TakeStats() {
    return { mOutDone.exchange(0), mInDone.exchange(0), mErrors.exchange(0) };
}

bool WinUsbBus::SubmitUSBPacket(uint8_t pipeID, void* buffer, uint32_t size, void* contextID) {
    int slot = PipeSlot(pipeID);
    WINUSB_INTERFACE_HANDLE h = mPipeIntf[slot];
    if (!h || mPipeAddr[slot] != pipeID) return false;

    Transfer* t = new Transfer();
    t->pipeID = pipeID;
    t->context = contextID;
    mPending++;
    BOOL ok = (pipeID & 0x80) ? WinUsb_ReadPipe(h, pipeID, (PUCHAR)buffer, size, nullptr, &t->ov)
                              : WinUsb_WritePipe(h, pipeID, (PUCHAR)buffer, size, nullptr, &t->ov);
    // Pending or done: either way the completion comes through the port
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
        Log("[WinUsbBus] pipe 0x%02x submit failed: %lu", pipeID, GetLastError());
        mPending--;
        delete t;
        return false;
    }
    return true;
}

bool WinUsbBus::SubmitIsoc(uint8_t pipeID, void*, uint32_t, OzzyIsocPacket*, uint32_t, void*) {
    static bool logged = false;
    if (!logged) {
        logged = true;
        Log("[WinUsbBus] isochronous pipe 0x%02x: not possible over WinUSB (no per-packet lengths)", pipeID);
    }
    return false;
}

bool WinUsbBus::VendorRequest(uint8_t type, uint8_t req, uint16_t val, uint16_t idx, void* data, uint16_t len) {
    WINUSB_SETUP_PACKET setup = { type, req, val, idx, len };
    ULONG done = 0;
    if (WinUsb_ControlTransfer(mIntf[0], setup, (PUCHAR)data, len, &done, nullptr)) return true;
    Log("[WinUsbBus] control request 0x%02x (type 0x%02x, wValue 0x%04x) failed: %lu", req, type, val, GetLastError());
    return false;
}

/* Deliver completions to the engine, one at a time. A failed transfer still completes through
 * the port: GetQueuedCompletionStatus returns FALSE with its OVERLAPPED, and the error (an
 * aborted pipe is ERROR_OPERATION_ABORTED, which the engines know as OZZY_STATUS_ABORTED). */
void WinUsbBus::CompletionThread() {
    MakeAudioThread();
    for (;;) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* ov = nullptr;
        BOOL ok = GetQueuedCompletionStatus(mPort, &bytes, &key, &ov, INFINITE);
        if (!ov) {
            if (!ok || key == kQuitKey) break;
            continue;
        }
        Transfer* t = CONTAINING_RECORD(ov, Transfer, ov);
        int status = ok ? 0 : (int)GetLastError();
        uint8_t pipeID = t->pipeID;
        void* context = t->context;
        delete t;

        if (status == 0) ((pipeID & 0x80) ? mInDone : mOutDone)++;
        else if (status != ERROR_OPERATION_ABORTED && mErrors++ < 10)
            Log("[WinUsbBus] pipe 0x%02x error: %d", pipeID, status);

        if (mEngine) mEngine->OnPacketComplete(pipeID, context, status, bytes);
        mPending--;
    }
}

bool WinUsbBus::StartTimer(uint32_t intervalUs) {
    if (mTimerOn.load()) return true;
    mTimerIntervalUs = intervalUs;
    mTimerOn = true;
    mTimer = std::thread(&WinUsbBus::TimerThread, this);
    return true;
}

void WinUsbBus::StopTimer() {
    mTimerOn = false;
    if (mTimer.joinable()) mTimer.join();   // waits for a call in progress
}

/* OzzyEngine::OnTimer every interval, on absolute deadlines (no drift); if we fell behind,
 * skip to the next deadline ahead, as KextBus does */
void WinUsbBus::TimerThread() {
    MakeAudioThread();
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (!timer) {
        Log("[WinUsbBus] no timer: %lu", GetLastError());
        return;
    }
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    int64_t interval = (int64_t)mTimerIntervalUs * freq.QuadPart / 1000000;
    int64_t deadline = (int64_t)GetTime() + interval;

    while (mTimerOn.load()) {
        int64_t now = (int64_t)GetTime();
        if (deadline > now) {
            LARGE_INTEGER due;
            due.QuadPart = -((deadline - now) * 10000000 / freq.QuadPart);   // relative, 100 ns
            if (due.QuadPart == 0) due.QuadPart = -1;
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, INFINITE);
        }
        if (!mTimerOn.load()) break;
        if (mEngine) mEngine->OnTimer();
        deadline += interval;
        now = (int64_t)GetTime();
        if (deadline <= now) deadline = now + interval;
    }
    CloseHandle(timer);
}

void WinUsbBus::Sleep(uint32_t ms) { ::Sleep(ms); }

uint64_t WinUsbBus::GetTime() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
}

void WinUsbBus::Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}
