#pragma once
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/IOUSBHostPipe.h>
#include <IOKit/IOBufferMemoryDescriptor.h> 
#include <IOKit/IOSubMemoryDescriptor.h> // REQUIRED for withSubRange
#include "../../../engine/core/ozzy_engine.h"
#include <kern/thread_call.h>

// The engine's bus (engine/core/ozzy_bus.h) on IOUSBHost: zero-copy transfers on slices of the
// shared memory descriptor, completions to the engine
class KextBus {
public:
    KextBus(IOUSBHostDevice* device, IOService* driver);
    ~KextBus();

    // What the engine runs on (ozzy_engine_attach)
    const struct ozzy_bus* Bus() const { return &mOzzyBus; }

    bool SubmitUSBPacket(uint8_t pipeID, void* buffer, uint32_t size, void* contextID);
    bool SubmitIsoc(uint8_t pipeID, void* buffer, uint32_t bufferSize,
                    struct ozzy_isoc_packet* packets, uint32_t count, void* contextID);
    bool VendorRequest(uint8_t type, uint8_t req, uint16_t val, uint16_t idx, void* data, uint16_t len);
    void Sleep(uint32_t ms);
    uint64_t GetTime();
    void SetEngine(struct ozzy_engine* engine);
    bool StartTimer(uint32_t intervalUs);
    void StopTimer();
    
    // NEW: Receive the Master Shared Memory Descriptor
    void SetSharedMemory(IOBufferMemoryDescriptor* mainDesc, void* virtBase);
    
    void SetPipePool(IOUSBHostPipe** pipes, int count);
    struct ozzy_engine* GetEngine();

    // Legacy Stub
    static void StaticCompletion(void* owner, void* parameter, IOReturn status, uint32_t bytesTransferred);

private:
    IOUSBHostDevice* mDevice;
    IOService* mDriver; 
    IOUSBHostPipe** mPipes;
    int mPipeCount;
    struct ozzy_engine* mEngine;
    struct ozzy_bus mOzzyBus;
    static const struct ozzy_bus_ops sOps;

    // Master Memory References
    IOBufferMemoryDescriptor* mMainShmDesc;
    uintptr_t mShmBase;

    // Endpoint number in bits 0-3, direction in bit 4
    static constexpr int kPipeSlots = 32;
    static int PipeSlot(uint8_t address) { return (address & 0x0F) | ((address & 0x80) >> 3); }
    IOUSBHostPipe* mPipeByAddress[kPipeSlots];

    IOUSBHostPipe* FindPipe(uint8_t address);
    IOMemoryDescriptor* IsocFrameSlice(uint8_t pipeID, void* buffer, uint32_t bufferSize,
                                       const struct ozzy_isoc_packet* packets, uint32_t count,
                                       IODirection dir);
    uint64_t IsocFrameNumber(IOUSBHostPipe* pipe, uint8_t pipeID, uint32_t count);

    // The 1 ms frame each pipe's next isochronous transfer starts on, the first this far ahead
    static constexpr uint64_t kIsocStartFrames = 10;
    uint64_t mIsocNextFrame[kPipeSlots];

    // Bus timer: a thread call re-armed on an absolute deadline every interval (no drift)
    static void TimerFired(thread_call_param_t bus, thread_call_param_t);
    thread_call_t mTimer = nullptr;
    uint64_t mTimerInterval = 0;     // absolute time units
    uint64_t mTimerDeadline = 0;
    volatile bool mTimerOn = false;
};