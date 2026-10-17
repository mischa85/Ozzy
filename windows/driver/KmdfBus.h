#ifndef KmdfBus_h
#define KmdfBus_h

// The engine's bus (engine/core/ozzy_bus.h) for the Windows ACX driver: the engine's transfers
// as KMDF USB requests on the shared memory, completed at DISPATCH_LEVEL. The engine encodes
// PCM out and decodes PCM in right there (ozzy_engine_set_pcm_ring), and the ACX streams hear
// about it through IOzzyPcmClient, so the real-time work happens in the completion path.

#include <ntddk.h>
#include <wdf.h>
#include <usb.h>
#include <wdfusb.h>
#include "../../engine/core/ozzy_engine.h"

// The ACX stream of one direction: told after every packet the engine moved through its ring
class IOzzyPcmClient {
public:
    // @frames: ring position after the packet; @qpc: KeQueryPerformanceCounter at completion
    virtual void OnPcmProgress(uint64_t frames, uint64_t qpc) = 0;
};

class KmdfBus {
public:
    KmdfBus();
    ~KmdfBus();

    // What the engine runs on (ozzy_engine_attach)
    const struct ozzy_bus* Bus() const { return &mOzzyBus; }

    // The USB target and its configuration (EvtDevicePrepareHardware)
    NTSTATUS Create(WDFDEVICE device);
    // Claim the profile's interfaces with its alternate setting, find each pipe, report it
    // with ozzy_engine_set_endpoint and create its requests on @shm (the transfers' memory)
    NTSTATUS Configure(struct ozzy_engine* engine, void* shm, size_t shmSize);

    // Cancel every request in flight and wait for them (the engine has stopped resubmitting);
    // StartIo makes the pipes usable again
    void StopIo();
    void StartIo();

    // The stream that hears about @dir's progress (nullptr: none)
    void SetPcmClient(enum ozzy_pcm_dir dir, IOzzyPcmClient* client);
    // Wait until no completion is in progress: after detaching a ring or client, before freeing it
    void WaitCompletions();

    bool SubmitUSBPacket(uint8_t pipeID, void* buffer, uint32_t size, void* contextID);
    bool VendorRequest(uint8_t type, uint8_t req, uint16_t val, uint16_t idx, void* data, uint16_t len);
    int ClearHalt(uint8_t address);
    bool StartTimer(uint32_t intervalUs);
    void StopTimer();
    void PcmProgress(enum ozzy_pcm_dir dir, uint64_t frames);
    void Sleep(uint32_t ms);
    uint64_t GetTime();   // KeQueryPerformanceCounter
    void Log(const char* fmt, ...);
    void SetEngine(struct ozzy_engine* engine) { mEngine = engine; }

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

    struct ozzy_engine* mEngine = nullptr;
    IOzzyPcmClient* volatile mClient[OZZY_PCM_DIRS] = {};
    struct ozzy_bus mOzzyBus;
    static const struct ozzy_bus_ops sOps;
    volatile LONG mInCompletion = 0;

    PEX_TIMER mTimer = nullptr;
};

#endif
