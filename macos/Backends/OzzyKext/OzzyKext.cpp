#include "OzzyKext.h"
#include "KextBus.h"
#include "../../Devices/Ploytec/PloytecEngine.h"
#include "../../Devices/UsbMidi/UsbMidiEngine.h"
#include "../../Shared/OzzySharedData.h"
#include "../../Shared/OzzyLog.h"
#include "../../../common/ozzy_version.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>

#include <IOKit/IOUserClient.h>

inline void* operator new(size_t, void* ptr) { return ptr; }

// --- OzzyUserClient Implementation ---

class OzzyUserClient : public IOUserClient {
    OSDeclareDefaultStructors(OzzyUserClient)

public:
    virtual bool initWithTask(task_t t, void* s, UInt32 type, OSDictionary* p) override {
        if (!IOUserClient::initWithTask(t, s, type, p)) return false;
        return true;
    }

    virtual bool start(IOService* provider) override {
        mOwner = OSDynamicCast(OzzyKext, provider);
        if (!mOwner) return false;
        return IOUserClient::start(provider);
    }

    // This is triggered by OzzyHAL calling IOConnectMapMemory
    virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits* options, 
                                         IOMemoryDescriptor** memory) override {
        // type 0 is what the HAL uses by default
        if (mOwner) {
            IOMemoryDescriptor* shm = mOwner->GetMemory();
            if (shm) {
                shm->retain(); // The framework will release it after the mapping is established
                *memory = shm;
                return kIOReturnSuccess;
            }
        }
        return kIOReturnNoMemory;
    }

    // Selector 0 (no arguments): OzzyMIDI queued bytes in the midiOut ring
    virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments* args,
                                    IOExternalMethodDispatch* dispatch, OSObject* target,
                                    void* reference) override {
        if (selector == kOzzyMethodMidiOutPending) {
            if (mOwner) mOwner->MidiOutPending();
            return kIOReturnSuccess;
        }
        return IOUserClient::externalMethod(selector, args, dispatch, target, reference);
    }

private:
    OzzyKext* mOwner = nullptr;
};

OSDefineMetaClassAndStructors(OzzyUserClient, IOUserClient)

// --- OzzyKext Implementation of newUserClient ---

IOReturn OzzyKext::newUserClient(task_t owningTask, void* securityID, 
                                 UInt32 type, OSDictionary* properties, 
                                 IOUserClient** handler) {
    OzzyUserClient* client = new OzzyUserClient;
    
    if (!client || !client->init()) {
        if (client) client->release();
        return kIOReturnNoMemory;
    }

    if (!client->attach(this)) {
        client->release();
        return kIOReturnError;
    }

    if (!client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnError;
    }

    *handler = client;
    return kIOReturnSuccess;
}

/* The Xone:1D is a USB MIDI class device; everything else speaks the Ploytec protocol.
 * @size returns the allocation size for SafeKernelDelete. */
OzzyEngine* CreateEngine(uint16_t vid, uint16_t pid, size_t* size) {
    bool usbMidi = (vid == PLOYTEC_VENDOR_ID && pid == PLOYTEC_PID_XONE_1D);
    *size = usbMidi ? sizeof(UsbMidiEngine) : sizeof(PloytecEngine);
    void* raw = IOMalloc(*size);
    if (!raw) return nullptr;
    memset(raw, 0, *size);
    if (usbMidi) return new(raw) UsbMidiEngine();
    return new(raw) PloytecEngine(vid, pid);
}

KextBus* CreateBus(IOUSBHostDevice* dev, IOService* driver) {
    void* raw = IOMalloc(sizeof(KextBus));
    if (!raw) return nullptr;
    memset(raw, 0, sizeof(KextBus));
    return new(raw) KextBus(dev, driver);
}

template <typename T>
void SafeKernelDelete(T* ptr, size_t size = sizeof(T)) {
    if (ptr) { ptr->~T(); IOFree(ptr, size); }
}

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static void GetRegistryString(IOService* service, const char* key, char* dest, size_t maxLen) {
    if (!service || !dest || maxLen == 0) return;
    dest[0] = '\0';
    
    OSObject* obj = service->copyProperty(key);
    if (!obj) return;
    
    OSString* str = OSDynamicCast(OSString, obj);
    if (str) {
        const char* cstr = str->getCStringNoCopy();
        if (cstr) {
            size_t len = 0;
            while (cstr[len] && len < maxLen - 1) {
                dest[len] = cstr[len];
                len++;
            }
            dest[len] = '\0';
        }
    }
    obj->release();
}

OSDefineMetaClassAndStructors(OzzyKext, IOService)

bool OzzyKext::start(IOService* provider) {
    if (!IOService::start(provider)) return false;
    LogOzzyKext("start (build %s)", OZZY_GIT_VERSION);

    mHostDevice = OSDynamicCast(IOUSBHostDevice, provider);
    if (!mHostDevice || !mHostDevice->open(this)) {
        LogOzzyKext("open failed");
        return false;
    }

    if (!MapSharedMemory()) return false;

    // Clear pools
    for(int i=0; i<kMaxPipes; i++) mPipes[i] = nullptr;
    for(int i=0; i<kMaxInterfaces; i++) mInterfaces[i] = nullptr;

    // Extract device metadata early so HAL can see it immediately
    const StandardUSB::DeviceDescriptor* desc = mHostDevice->getDeviceDescriptor();
    if (desc) {
        mSHM->vendorID = desc->idVendor;
        mSHM->productID = desc->idProduct;
        
        // Get device strings from IORegistry
        GetRegistryString(mHostDevice, "USB Vendor Name", mSHM->manufacturerName, 64);
        GetRegistryString(mHostDevice, "USB Product Name", mSHM->productName, 64);
        GetRegistryString(mHostDevice, "USB Serial Number", mSHM->serialNumber, 64);
        
        LogOzzyKext("found device: %s %s (S/N: %s)", mSHM->manufacturerName, mSHM->productName,
            mSHM->serialNumber[0] ? mSHM->serialNumber : "none");
        LogOzzyKext("  USB ID: %04x:%04x", desc->idVendor, desc->idProduct);
        LogOzzyKext("  speed: %s",
            mHostDevice->getSpeed() == kUSBHostConnectionSpeedHigh ? "high" :
            mHostDevice->getSpeed() == kUSBHostConnectionSpeedFull ? "full" : "unknown");
    }
    
    uint16_t vid = desc ? desc->idVendor : 0;
    uint16_t pid = desc ? desc->idProduct : 0;
    
    mEngine = CreateEngine(vid, pid, &mEngineSize);
    mBus = CreateBus(mHostDevice, this);

    if (!mEngine || !mBus) return false;

    mBus->SetEngine(mEngine);
    
    // NEW: Hand the Master Descriptor to the Bus
    mBus->SetSharedMemory(mShmDescriptor, mSHM);

    LogOzzyKext("configuring hardware...");
    if (!ConfigureHardware()) {
        LogOzzyKext("configuration failed");
        return false;
    }

    mBus->SetPipePool(mPipes, kMaxPipes);

    LogOzzyKext("initializing engine...");
    mEngine->Init(mBus, mSHM);
    
    if (!mEngine->Start()) {
        LogOzzyKext("engine start failed");
        return false;
    }

    LogOzzyKext("driver ready");

    // OzzyHAL only attaches to devices with audio (the Xone:1D is MIDI only)
    setProperty(kOzzyAudioPropertyKey, mSHM->audio.inputChannels + mSHM->audio.outputChannels > 0);
    mRunning = true;

    registerService();

    return true;
}

void OzzyKext::MidiOutPending() {
    if (mRunning && mEngine) mEngine->OnMidiOutPending();
}

IOReturn OzzyKext::message(UInt32 type, IOService* provider, void* argument) {
    if (type == kIOMessageServiceIsTerminated) {
        LogOzzyKext("device disconnected");
        mRunning = false;
        
        // Immediately signal HAL that hardware is gone
        if (mSHM) {
            mSHM->audio.hardwarePresent = false;
            mSHM->audio.driverReady = false;
        }
        
        // CRITICAL: Stop engine and abort transfers NOW to prevent callback deadlocks
        if (mEngine) {
            mEngine->Stop();
        }
        
        // Close the device and interfaces now: IOKit only calls stop() once we have closed
        // our provider, so closing them in stop() leaves the device and both interfaces busy
        // forever. After a sleep/wake that becomes a 60 s registry busy timeout panic (#52).
        CloseProviders();
    }
    
    // MUST call super - failure to do so causes termination timeout and kernel panic
    return IOService::message(type, provider, argument);
}

/* Abort all transfers, then close the interfaces and the device. Safe to call twice. */
void OzzyKext::CloseProviders() {
    for (int i = 0; i < kMaxPipes; i++) {
        if (mPipes[i]) {
            mPipes[i]->abort();
        }
    }
    for (int i = 0; i < kMaxInterfaces; i++) {
        if (mInterfaces[i] && mInterfaces[i]->isOpen(this)) {
            mInterfaces[i]->close(this);
        }
    }
    if (mHostDevice && mHostDevice->isOpen(this)) {
        mHostDevice->close(this);
    }
}

void OzzyKext::stop(IOService* provider) {
    LogOzzyKext("stop");
    
    // 0. Invalidate shared memory magic to signal HAL immediately
    if (mSHM) {
        mSHM->magic = 0;
    }
    
    // 1. Stop the engine first (clears flags, signals HAL)
    mRunning = false;
    if (mEngine) { 
        mEngine->Stop();
    }
    
    // 2. Abort all pending USB transfers and close interfaces and device (if not done on termination)
    CloseProviders();
    
    // 3. Give USB stack time to complete callbacks
    IOSleep(100);
    
    // 4. Now safe to release pipes
    for (int i = 0; i < kMaxPipes; i++) {
        if (mPipes[i]) { 
            mPipes[i]->release(); 
            mPipes[i] = nullptr; 
        }
    }
    
    // 5. Release interfaces (closed in CloseProviders)
    for (int i = 0; i < kMaxInterfaces; i++) {
        if (mInterfaces[i]) { 
            mInterfaces[i]->release(); 
            mInterfaces[i] = nullptr; 
        }
    }
    
    // 6. Clean up engine and bus
    if (mEngine) { 
        SafeKernelDelete(mEngine, mEngineSize); 
        mEngine = nullptr; 
    }
    if (mBus) { 
        SafeKernelDelete(mBus); 
        mBus = nullptr; 
    }
    
    // 7. Unmap shared memory
    UnmapSharedMemory();
    
    // 8. Forget the USB device (closed in CloseProviders; don't release - it's the provider)
    mHostDevice = nullptr;
    
    IOService::stop(provider);
}

void OzzyKext::free() {
    LogOzzyKext("free");
    IOService::free();
}

bool OzzyKext::MapSharedMemory() {
    // FIX: Store the descriptor in member variable, don't leak it!
    mShmDescriptor = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIOMemoryPhysicallyContiguous, sizeof(OzzySharedMemory), 0);
    
    if (!mShmDescriptor) return false;
    
    mShmDescriptor->prepare();
    mShmMap = mShmDescriptor->map();
    mSHM = (OzzySharedMemory*)mShmMap->getVirtualAddress();
    memset(mSHM, 0, sizeof(OzzySharedMemory));
    mSHM->magic = kOzzyMagic;
    mSHM->version = kOzzySharedVersion;
    
    // Generate random session ID using current time and address
    uint32_t random = (uint32_t)((uintptr_t)mSHM ^ mach_absolute_time());
    mSHM->sessionID = random;
    
    return true;
}

void OzzyKext::UnmapSharedMemory() { 
    if (mShmMap) { mShmMap->release(); mShmMap = nullptr; } 
    if (mShmDescriptor) { mShmDescriptor->release(); mShmDescriptor = nullptr; } // Release Master
    mSHM = nullptr; 
}

bool OzzyKext::ConfigureHardware() {
    // 1. Get WorkLoop
    IOWorkLoop* wl = getWorkLoop();
    if (!wl) { LogOzzyKext("no workloop"); return false; }

    const OzzyDeviceProfile& profile = mEngine->GetProfile();

    mHostDevice->setConfiguration(1);
    IOSleep(500); 

    const IORegistryPlane* plane = getPlane("IOService");
    int foundCount = 0;
    
    OSIterator* it = mHostDevice->getChildIterator(plane);
    if (it) {
        OSObject* obj = nullptr;
        while ((obj = it->getNextObject())) {
            IOUSBHostInterface* iface = OSDynamicCast(IOUSBHostInterface, obj);
            if (!iface) continue;
            const StandardUSB::InterfaceDescriptor* d = iface->getInterfaceDescriptor();
            if (!d) continue;

            for(int i=0; i < profile.interfaceCount; i++) {
                if (d->bInterfaceNumber == profile.interfaceIndices[i] && !mInterfaces[i]) {
                    if (iface->open(this)) {
                        iface->retain();
                        mInterfaces[i] = iface;
                        foundCount++;
                        if (profile.altSetting) {
                            iface->selectAlternateSetting(profile.altSetting);
                            IOSleep(50);
                        }
                    }
                }
            }
        }
        it->release();
    }

    if (foundCount != profile.interfaceCount) return false;

    int pipeCounter = 0;
    OzzyPipeConfig* configs[] = {
        const_cast<OzzyPipeConfig*>(&profile.pcmOut),
        const_cast<OzzyPipeConfig*>(&profile.pcmIn),
        const_cast<OzzyPipeConfig*>(&profile.midiIn),
        const_cast<OzzyPipeConfig*>(&profile.midiOut)
    };

    for (int k = 0; k < 4; k++) {
        OzzyPipeConfig* cfg = configs[k];
        if (cfg->address == 0) continue;

        IOUSBHostInterface* targetIntf = nullptr;
        for(int i=0; i < profile.interfaceCount; i++) {
            if (profile.interfaceIndices[i] == cfg->interfaceIndex) {
                targetIntf = mInterfaces[i];
                break;
            }
        }

        if (targetIntf) {
            IOUSBHostPipe* p = targetIntf->copyPipe(cfg->address);
            if (p) {
                if (pipeCounter < kMaxPipes) {
                    mPipes[pipeCounter++] = p;
                    const StandardUSB::EndpointDescriptor* d = p->getEndpointDescriptor();
                    LogOzzyKext("  pipe 0x%02x opened (wMaxPacketSize %u)", cfg->address,
                                d ? USBToHost16(d->wMaxPacketSize) & 0x07FF : 0);

                    // Pass transfer type and wMaxPacketSize of the output endpoint to the engine
                    if (k == 0) {
                        const StandardUSB::EndpointDescriptor* epDesc = p->getEndpointDescriptor();
                        if (epDesc) {
                            bool isBulk = (epDesc->bmAttributes & 0x03) == 0x02;
                            uint16_t maxPacketSize = USBToHost16(epDesc->wMaxPacketSize) & 0x07FF;
                            mEngine->SetOutEndpoint(isBulk, maxPacketSize);
                        }
                    }
                } else p->release();
            } else {
                LogOzzyKext("  failed to open pipe 0x%02x", cfg->address);
            }
        } else {
            LogOzzyKext("  no interface found for pipe 0x%02x", cfg->address);
        }
    }
    return true;
}

extern "C" {
    kern_return_t OzzyKext_start(kmod_info_t * ki, void *d) { return KERN_SUCCESS; }
    kern_return_t OzzyKext_stop(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
    __attribute__((visibility("default"))) kmod_start_func_t *_realmain = OzzyKext_start;
    __attribute__((visibility("default"))) kmod_stop_func_t  *_antimain = OzzyKext_stop;
    __attribute__((visibility("default"))) kmod_info_t kmod_info = {
        0, KMOD_INFO_VERSION, 0, "com.ozzy.kext.OzzyKext", "1.0.0", -1, 0, 0, 0, 0, OzzyKext_start, OzzyKext_stop
    };
}