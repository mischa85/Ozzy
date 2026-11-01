#ifndef OzzyKext_h
#define OzzyKext_h

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/usb/IOUSBHostPipe.h>
#include <IOKit/IOBufferMemoryDescriptor.h> // Added include
#include <IOKit/IOLocks.h>
#include <kern/thread_call.h>

class KextBus;
struct ozzy_engine;
struct ozzy_shared_memory;

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
    virtual IOReturn setPowerState(unsigned long state, IOService* device) override;

    IOMemoryDescriptor* GetMemory() { return mShmDescriptor; }
    void MidiOutPending();
    IOReturn ApplySetting(uint32_t id, uint32_t value);
    IOReturn SetRate(uint32_t rate);
    ozzy_shared_memory* GetSHM() { return mSHM; }
    bool Reselect(const uint8_t alts[4]);   // the bus's reselect (ozzy_engine_reselect), engine suspended

private:
    bool MapSharedMemory();
    void UnmapSharedMemory();
    bool ConfigureHardware();
    void CloseProviders();
    IOReturn ApplySettingLocked(uint32_t id, uint32_t value);
    IOReturn RestartLocked();
    void ReleasePipes();
    bool CopyPipes();

    // Stall watchdog: a PCM pipe without a completion for kWatchDeadMs while streaming has stopped
    // the device (the WZ4 after OUT ran dry, #85); a reselect and a new start bring it back
    static void WatchFired(thread_call_param_t driver, thread_call_param_t);
    void StartWatch();
    void StopWatch();
    void WatchLocked();
    thread_call_t mWatch = nullptr;
    volatile bool mWatchOn = false;
    volatile bool mWatchArmed = false;   // as KextBus's timer: one of them set from enter to return
    volatile bool mWatchBusy = false;
    uint64_t mRecoveredAt = 0;           // mach time of the last recovery (0: none)
    bool mSleeping = false;              // suspended for system sleep (setPowerState)
    bool mPMInited = false;
    static const int kPowerStates = 2;
    static IOPMPowerState sPowerStates[kPowerStates];

    IOUSBHostDevice* mHostDevice = nullptr;
    KextBus* mBus = nullptr;
    struct ozzy_engine* mEngine = nullptr;
    size_t mEngineSize = 0;
    volatile bool mRunning = false;   // engine started and device present
    IOLock* mSettingsLock = nullptr;  // serializes ApplySetting with itself and termination
    
    // MASTER MEMORY OBJECTS
    IOBufferMemoryDescriptor* mShmDescriptor = nullptr; // The Owner
    IOMemoryMap* mShmMap = nullptr;                     // The Mapping
    ozzy_shared_memory* mSHM = nullptr;                   // The Pointer

    IOUSBHostInterface* mInterfaces[kMaxInterfaces] = { nullptr };
    IOUSBHostPipe* mPipes[kMaxPipes]           = { nullptr };
    // What each of mPipes is, to open it again after a reselect: its interface (index into
    // mInterfaces) and endpoint address
    uint8_t mPipeInterface[kMaxPipes]          = { 0 };
    uint8_t mPipeAddress[kMaxPipes]            = { 0 };
    int mPipeCount                             = 0;
};

#endif