#pragma once
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/IOUSBHostPipe.h>
#include <IOKit/IOBufferMemoryDescriptor.h> 
#include <IOKit/IOSubMemoryDescriptor.h> // REQUIRED for withSubRange
#include "../../Devices/Ploytec/PloytecBus.h"

class KextBus : public PloytecBus {
public:
    KextBus(IOUSBHostDevice* device, IOService* driver);
    virtual ~KextBus();

    // IOzzyBus Implementation
    virtual bool SubmitUSBPacket(uint8_t pipeID, void* buffer, uint32_t size, void* contextID) override;
    virtual bool VendorRequest(uint8_t type, uint8_t req, uint16_t val, uint16_t idx, void* data, uint16_t len) override;
    
    virtual void Sleep(uint32_t ms) override;
    virtual uint64_t GetTime() override;
    virtual void Log(const char* fmt, ...) override;
    virtual void SetEngine(OzzyEngine* engine) override;
    
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
};