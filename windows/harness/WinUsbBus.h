#ifndef WinUsbBus_h
#define WinUsbBus_h

// IOzzyBus over WinUSB, for the Windows dev harness: the engine runs in a console program and
// talks to a device bound to WinUsb.sys (e.g. with Zadig). Not shipped; the driver is ACX.
//
// Transfers are overlapped I/O on the device handle, completed on one thread through an I/O
// completion port, so engine callbacks never run concurrently (as on the kext's work loop).
// Isochronous OUT is not possible: WinUSB packetizes a write at the full bytes per interval
// and takes no per-packet lengths, which feedback pacing needs (the ACX driver uses URBs).

#include <windows.h>
#include <winusb.h>
#include <usb.h>
#include <atomic>
#include <thread>
#include "../../engine/core/OzzyEngine.h"

class WinUsbBus : public IOzzyBus {
public:
    WinUsbBus() {}
    virtual ~WinUsbBus();

    // Open the device at @path (a WinUSB device interface path) and start the completion thread
    bool Open(const wchar_t* path);
    // Abort every pipe, wait for outstanding transfers, close everything. Safe to call twice.
    void Close();

    // What OzzyKext::ConfigureHardware does: claim the profile's interfaces, select the
    // alternate setting, find each pipe and report it with OzzyEngine::SetEndpoint
    bool Configure(OzzyEngine* engine);

    // String descriptor @index (US English) as ASCII into @dst; false if there is none
    bool GetString(uint8_t index, char* dst, size_t size);
    const USB_DEVICE_DESCRIPTOR& DeviceDescriptor() const { return mDeviceDesc; }

    // Completed transfers and failures per direction since the last call (harness stats)
    struct Stats { uint32_t outDone, inDone, errors; };
    Stats TakeStats();

    // --- IOzzyBus ---
    bool SubmitUSBPacket(uint8_t pipeID, void* buffer, uint32_t size, void* contextID) override;
    bool SubmitIsoc(uint8_t pipeID, void* buffer, uint32_t bufferSize,
                    OzzyIsocPacket* packets, uint32_t count, void* contextID) override;
    bool VendorRequest(uint8_t type, uint8_t req, uint16_t val, uint16_t idx, void* data, uint16_t len) override;
    bool StartTimer(uint32_t intervalUs) override;
    void StopTimer() override;
    void Sleep(uint32_t ms) override;
    uint64_t GetTime() override;   // QueryPerformanceCounter ticks
    void Log(const char* fmt, ...) override;
    void SetEngine(OzzyEngine* engine) override { mEngine = engine; }

private:
    struct Transfer {
        OVERLAPPED ov;        // first: the completion port hands back &ov
        uint8_t pipeID;
        void* context;
    };

    static constexpr int kMaxInterfaces = 8;
    static constexpr int kPipeSlots = 32;
    // Endpoint number in bits 0-3, direction in bit 4 (as KextBus)
    static int PipeSlot(uint8_t address) { return (address & 0x0F) | ((address & 0x80) >> 3); }

    void CompletionThread();
    void TimerThread();

    HANDLE mFile = INVALID_HANDLE_VALUE;
    HANDLE mPort = nullptr;
    WINUSB_INTERFACE_HANDLE mIntf[kMaxInterfaces] = {};   // [0] from WinUsb_Initialize
    uint8_t mIntfNumber[kMaxInterfaces] = {};             // bInterfaceNumber of each handle
    int mIntfCount = 0;
    WINUSB_INTERFACE_HANDLE mPipeIntf[kPipeSlots] = {};   // interface handle per endpoint
    uint8_t mPipeAddr[kPipeSlots] = {};
    USB_DEVICE_DESCRIPTOR mDeviceDesc = {};

    OzzyEngine* mEngine = nullptr;
    std::thread mCompletion;
    std::atomic<int> mPending{0};
    std::atomic<uint32_t> mOutDone{0}, mInDone{0}, mErrors{0};

    std::thread mTimer;
    std::atomic<bool> mTimerOn{false};
    uint32_t mTimerIntervalUs = 0;
};

#endif
