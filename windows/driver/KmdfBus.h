#ifndef KmdfBus_h
#define KmdfBus_h

// IOzzyBus for the Windows ACX driver: the engine's transfers as KMDF USB requests on the
// shared memory, completed at DISPATCH_LEVEL. The engine encodes PCM out and decodes PCM in
// right there (OzzyEngine::SetPcmRing), and the ACX streams hear about it through
// IOzzyPcmClient, so the real-time work happens in the completion path.

#include <ntddk.h>
#include <wdf.h>
#include <usb.h>
#include <wdfusb.h>
#include "../../engine/core/OzzyEngine.h"

// The ACX stream of one direction: told after every packet the engine moved through its ring
class IOzzyPcmClient {
public:
    // @frames: ring position after the packet; @qpc: KeQueryPerformanceCounter at completion
    virtual void OnPcmProgress(uint64_t frames, uint64_t qpc) = 0;
};

class KmdfBus : public IOzzyBus {
public:
    ~KmdfBus();

    // The USB target and its configuration (EvtDevicePrepareHardware)
    NTSTATUS Create(WDFDEVICE device);
    // Claim the profile's interfaces with its alternate setting, find each pipe, report it
    // with OzzyEngine::SetEndpoint and create its requests on @shm (the transfers' memory)
    NTSTATUS Configure(OzzyEngine* engine, void* shm, size_t shmSize);

    // Cancel every request in flight and wait for them (the engine has stopped resubmitting);
    // StartIo makes the pipes usable again
    void StopIo();
    void StartIo();

    // The stream that hears about @dir's progress (nullptr: none)
    void SetPcmClient(OzzyPcmDir dir, IOzzyPcmClient* client);
    // Wait until no completion is in progress: after detaching a ring or client, before freeing it
    void WaitCompletions();

    // --- IOzzyBus ---
    bool SubmitUSBPacket(uint8_t pipeID, void* buffer, uint32_t size, void* contextID) override;
    bool SubmitIsoc(uint8_t pipeID, void* buffer, uint32_t bufferSize,
                    OzzyIsocPacket* packets, uint32_t count, void* contextID) override;
    bool VendorRequest(uint8_t type, uint8_t req, uint16_t val, uint16_t idx, void* data, uint16_t len) override;
    bool StartTimer(uint32_t intervalUs) override;
    void StopTimer() override;
    void PcmProgress(int dir, uint64_t frames) override;
    void Sleep(uint32_t ms) override;
    uint64_t GetTime() override;   // KeQueryPerformanceCounter
    void Log(const char* fmt, ...) override;
    void SetEngine(OzzyEngine* engine) override { mEngine = engine; }

private:
    static constexpr int kMaxPipes = 8;
    static constexpr int kRequestsPerPipe = 4;   // the engines keep at most 2 per pipe in flight

    struct Slot {                 // one request's completion context
        KmdfBus* bus;
        uint8_t pipe;             // index in mPipes
        void* context;            // the engine's
    };
    struct Pipe {
        WDFUSBPIPE pipe;
        uint8_t address;
        WDFREQUEST requests[kRequestsPerPipe];
        Slot slots[kRequestsPerPipe];
        volatile LONG busy;       // bit per request in flight
    };

    static EVT_WDF_REQUEST_COMPLETION_ROUTINE Completion;
    static EXT_CALLBACK TimerCallback;

    Pipe* FindPipe(uint8_t address);
    const USB_ENDPOINT_DESCRIPTOR* FindEndpoint(uint8_t intf, uint8_t alt, uint8_t address) const;
    NTSTATUS AddPipe(WDFUSBPIPE pipe, uint8_t address);

    WDFDEVICE mDevice = nullptr;
    WDFUSBDEVICE mUsb = nullptr;
    PUSB_CONFIGURATION_DESCRIPTOR mConfig = nullptr;   // for the raw endpoint descriptors
    WDFMEMORY mShm = nullptr;
    uint8_t* mShmBase = nullptr;
    Pipe mPipes[kMaxPipes] = {};
    int mPipeCount = 0;

    OzzyEngine* mEngine = nullptr;
    IOzzyPcmClient* volatile mClient[kOzzyPcmDirs] = {};
    volatile LONG mInCompletion = 0;

    PEX_TIMER mTimer = nullptr;
};

#endif
