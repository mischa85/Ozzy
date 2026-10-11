#ifndef OzzyKext_h
#define OzzyKext_h

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/usb/IOUSBHostPipe.h>
#include <IOKit/IOBufferMemoryDescriptor.h> // Added include

class KextBus;
class OzzyEngine;
struct OzzySharedMemory;

#define kMaxInterfaces 4
#define kMaxPipes      16

class OzzyKext : public IOService {
    OSDeclareDefaultStructors(OzzyKext)

public:
    virtual bool start(IOService* provider) override;
    virtual void stop(IOService* provider) override;
   	virtual IOReturn newUserClient(task_t t, void* sec, UInt32 type, OSDictionary* p, IOUserClient** h) override;
    virtual void free() override; // Added free
    virtual IOReturn message(UInt32 type, IOService* provider, void* argument) override;

    IOMemoryDescriptor* GetMemory() { return mShmDescriptor; }
    void MidiOutPending();
    OzzySharedMemory* GetSHM() { return mSHM; }

private:
    bool MapSharedMemory();
    void UnmapSharedMemory();
    bool ConfigureHardware();
    void CloseProviders();

    IOUSBHostDevice* mHostDevice = nullptr;
    KextBus* mBus = nullptr;
    OzzyEngine* mEngine = nullptr;
    size_t mEngineSize = 0;
    volatile bool mRunning = false;   // engine started and device present
    
    // MASTER MEMORY OBJECTS
    IOBufferMemoryDescriptor* mShmDescriptor = nullptr; // The Owner
    IOMemoryMap* mShmMap = nullptr;                     // The Mapping
    OzzySharedMemory* mSHM = nullptr;                   // The Pointer

    IOUSBHostInterface* mInterfaces[kMaxInterfaces] = { nullptr };
    IOUSBHostPipe* mPipes[kMaxPipes]           = { nullptr };
};

#endif