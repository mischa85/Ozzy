#pragma once
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/IOUSBHostPipe.h>
#include <IOKit/IOBufferMemoryDescriptor.h> 
#include <IOKit/IOSubMemoryDescriptor.h> // REQUIRED for withSubRange
#include "../../Devices/Ploytec/PloytecBus.h"
#include <kern/thread_call.h>

class KextBus : public PloytecBus {
public:
    KextBus(IOUSBHostDevice* device, IOService* driver);
    virtual ~KextBus();

    // IOzzyBus Implementation
    virtual bool SubmitUSBPacket(uint8_t pipeID, void* buffer, uint32_t size, void* contextID) override;
    virtual bool SubmitIsoc(uint8_t pipeID, void* buffer, uint32_t bufferSize,
                            OzzyIsocPacket* packets, uint32_t count, void* contextID) override;
    virtual bool VendorRequest(uint8_t type, uint8_t req, uint16_t val, uint16_t idx, void* data, uint16_t len) override;
    
    virtual void Sleep(uint32_t ms) override;
    virtual uint64_t GetTime() override;
    virtual void Log(const char* fmt, ...) override;
    virtual void SetEngine(OzzyEngine* engine) override;
    virtual bool StartTimer(uint32_t intervalUs) override;
    virtual void StopTimer() override;
    
    // NEW: Receive the Master Shared Memory Descriptor
    void SetSharedMemory(IOBufferMemoryDescriptor* mainDesc, void* virtBase);
    
    void SetPipePool(IOUSBHostPipe** pipes, int count);
    OzzyEngine* GetEngine();

    // Legacy Stub
    static void StaticCompletion(void* owner, void* parameter, IOReturn status, uint32_t bytesTransferred);

private:
    IOUSBHostDevice* mDevice;
    IOService* mDriver; 
    IOUSBHostPipe** mPipes;
    int mPipeCount;
    OzzyEngine* mEngine;

    // Master Memory References
    IOBufferMemoryDescriptor* mMainShmDesc;
    uintptr_t mShmBase;

    // Endpoint number in bits 0-3, direction in bit 4
    static constexpr int kPipeSlots = 32;
    static int PipeSlot(uint8_t address) { return (address & 0x0F) | ((address & 0x80) >> 3); }
    IOUSBHostPipe* mPipeByAddress[kPipeSlots];

    IOUSBHostPipe* FindPipe(uint8_t address);

    // Bus timer: a thread call re-armed on an absolute deadline every interval (no drift)
    static void TimerFired(thread_call_param_t bus, thread_call_param_t);
    thread_call_t mTimer = nullptr;
    uint64_t mTimerInterval = 0;     // absolute time units
    uint64_t mTimerDeadline = 0;
    volatile bool mTimerOn = false;
};