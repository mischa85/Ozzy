#include "OzzyKext.h"
#include "KextBus.h"
#include "../../Devices/Ploytec/PloytecEngine.h"
#include "../../Devices/Ploytec/PloytecIsocEngine.h"
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
    // Selector 1 (in: setting id, value): OzzyHAL changes a setting
    virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments* args,
                                    IOExternalMethodDispatch* dispatch, OSObject* target,
                                    void* reference) override {
        if (selector == kOzzyMethodMidiOutPending) {
            if (mOwner) mOwner->MidiOutPending();
            return kIOReturnSuccess;
        }
        if (selector == kOzzyMethodApplySetting) {
            if (!mOwner || args->scalarInputCount != 2) return kIOReturnBadArgument;
            if (args->scalarInput[0] > 0xFFFF || args->scalarInput[1] > 0xFF) return kIOReturnBadArgument;
            return mOwner->ApplySetting((uint32_t)args->scalarInput[0], (uint32_t)args->scalarInput[1]);
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

/* The engine for a device's family (common/ozzy_devices.def). The kext's personalities are
 * generated from the same list, so the lookup fails only for a device that isn't in it.
 * @size returns the allocation size for SafeKernelDelete. */
OzzyEngine* CreateEngine(uint16_t vid, uint16_t pid, size_t* size) {
    const struct ozzy_device* device = ozzy_device_lookup(vid, pid);
    if (!device) {
        LogOzzyKext("%04x:%04x is not in the device list", vid, pid);
        return nullptr;
    }
    LogOzzyKext("  device: %s%s", device->name, device->status == OZZY_STATUS_EXPERIMENTAL ? " (experimental)" : "");

    switch (device->family) {
    case OZZY_FAMILY_USB_MIDI:     *size = sizeof(UsbMidiEngine); break;
    case OZZY_FAMILY_PLOYTEC_ISOC: *size = sizeof(PloytecIsocEngine); break;
    default:                       *size = sizeof(PloytecEngine); break;
    }
    void* raw = IOMalloc(*size);
    if (!raw) return nullptr;
    memset(raw, 0, *size);
    switch (device->family) {
    case OZZY_FAMILY_USB_MIDI:     return new(raw) UsbMidiEngine();
    case OZZY_FAMILY_PLOYTEC_ISOC: return new(raw) PloytecIsocEngine(device);
    default:                       return new(raw) PloytecEngine(device);
    }
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
    mSettingsLock = IOLockAlloc();
    if (!mSettingsLock) return false;

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

/* kOzzyMethodApplySetting: restart the engine with setting @id = @value, the way the official
 * driver suspends streaming, sends the setting and resumes. The engine stops resubmitting, every
 * pipe is aborted, and Start() runs the handshake again with the setting (it also re-reads the
 * digital lock, so the same value again re-checks it). hardwarePresent and driverReady stay set,
 * so the HAL keeps the device; it calls this with IO stopped. */
IOReturn OzzyKext::ApplySetting(uint32_t id, uint32_t value) {
    if (!mSettingsLock) return kIOReturnNotReady;
    IOLockLock(mSettingsLock);
    IOReturn ret = ApplySettingLocked(id, value);
    IOLockUnlock(mSettingsLock);
    return ret;
}

IOReturn OzzyKext::ApplySettingLocked(uint32_t id, uint32_t value) {
    if (!mRunning || !mEngine) return kIOReturnNotReady;
    // Device settings only: the HAL applies routes itself
    if (!mEngine->HasSetting(id) || ozzy_setting_descs[id].kind != OZZY_SETTING_DEVICE ||
        value >= ozzy_setting_descs[id].num_values)
        return kIOReturnBadArgument;

    LogOzzyKext("%s: %s", ozzy_setting_descs[id].name, ozzy_setting_descs[id].value_names[value]);
    mEngine->Suspend();
    for (int i = 0; i < kMaxPipes; i++) {
        if (mPipes[i]) mPipes[i]->abort(IOUSBHostIOSource::kAbortSynchronous);
    }
    mEngine->SetSetting(id, (uint8_t)value);
    if (!mEngine->Start()) {
        // The device didn't take it: let the HAL drop it, as on a failed start
        LogOzzyKext("engine restart failed");
        mRunning = false;
        mEngine->Stop();
        return kIOReturnIOError;
    }
    return kIOReturnSuccess;
}

IOReturn OzzyKext::message(UInt32 type, IOService* provider, void* argument) {
    if (type == kIOMessageServiceIsTerminated) {
        LogOzzyKext("device disconnected");
        // Wait for a settings restart in progress (its requests fail fast once the device is gone)
        if (mSettingsLock) IOLockLock(mSettingsLock);
        mRunning = false;
        if (mSettingsLock) IOLockUnlock(mSettingsLock);
        
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
    if (mSettingsLock) {
        IOLockFree(mSettingsLock);
        mSettingsLock = nullptr;
    }
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

/* Every endpoint of an interface's current alt setting, so a debug log doubles as the
 * descriptor dump for devices nobody captured yet */
static void LogEndpoints(IOUSBHostInterface* iface) {
    const StandardUSB::ConfigurationDescriptor* cfg = iface->getConfigurationDescriptor();
    const StandardUSB::InterfaceDescriptor* intf = iface->getInterfaceDescriptor();
    static const char* const types[] = { "control", "isoc", "bulk", "interrupt" };
    if (!cfg || !intf) return;
    LogOzzyKext("  interface %u alt %u: class 0x%02x, %u endpoints", intf->bInterfaceNumber,
                intf->bAlternateSetting, intf->bInterfaceClass, intf->bNumEndpoints);
    const StandardUSB::EndpointDescriptor* ep = nullptr;
    while ((ep = StandardUSB::getNextEndpointDescriptor(cfg, intf, ep))) {
        uint16_t w = USBToHost16(ep->wMaxPacketSize);
        LogOzzyKext("    EP 0x%02x %s %s wMaxPacketSize %u x%u bInterval %u", ep->bEndpointAddress,
                    (ep->bEndpointAddress & 0x80) ? "IN" : "OUT", types[ep->bmAttributes & 3],
                    w & 0x7FF, ((w >> 11) & 3) + 1, ep->bInterval);
    }
}

/* The first endpoint on @iface's current alt setting that @spec searches for
 * (ozzy_ep_matches, shared with Linux), or 0 */
static uint8_t FindEndpoint(IOUSBHostInterface* iface, const struct ozzy_ep* spec) {
    const StandardUSB::ConfigurationDescriptor* cfg = iface->getConfigurationDescriptor();
    const StandardUSB::InterfaceDescriptor* intf = iface->getInterfaceDescriptor();
    if (!cfg || !intf) return 0;
    const StandardUSB::EndpointDescriptor* ep = nullptr;
    while ((ep = StandardUSB::getNextEndpointDescriptor(cfg, intf, ep))) {
        if (ozzy_ep_matches(spec, ep->bEndpointAddress, ep->bmAttributes))
            return ep->bEndpointAddress;
    }
    return 0;
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
    for (int i = 0; i < profile.interfaceCount; i++) LogEndpoints(mInterfaces[i]);

    int pipeCounter = 0;
    const OzzyPipeConfig* configs[kOzzyPipeRoles] = {
        &profile.pcmOut, &profile.pcmIn, &profile.midiIn, &profile.midiOut, &profile.feedback
    };

    for (int k = 0; k < kOzzyPipeRoles; k++) {
        const OzzyPipeConfig* cfg = configs[k];
        if (cfg->address == 0 && cfg->types == 0) continue;

        IOUSBHostInterface* targetIntf = nullptr;
        for(int i=0; i < profile.interfaceCount; i++) {
            if (profile.interfaceIndices[i] == cfg->intf) {
                targetIntf = mInterfaces[i];
                break;
            }
        }

        uint8_t address = cfg->address;
        if (targetIntf && !address) {
            address = FindEndpoint(targetIntf, cfg);
            if (!address) {
                LogOzzyKext("  no endpoint of the wanted type on interface %u (pipe role %d)", cfg->intf, k);
                continue;
            }
        }

        if (targetIntf) {
            IOUSBHostPipe* p = targetIntf->copyPipe(address);
            if (p) {
                if (pipeCounter < kMaxPipes) {
                    mPipes[pipeCounter++] = p;
                    const StandardUSB::EndpointDescriptor* d = p->getEndpointDescriptor();
                    // Bytes per service interval: wMaxPacketSize times the High Speed high-bandwidth multiplier
                    uint16_t w = d ? USBToHost16(d->wMaxPacketSize) : 0;
                    uint16_t maxPacketSize = (w & 0x07FF) * (((w >> 11) & 3) + 1);
                    LogOzzyKext("  pipe 0x%02x opened (wMaxPacketSize %u)", address, maxPacketSize);
                    if (d) mEngine->SetEndpoint((OzzyPipeRole)k, address, d->bmAttributes & 0x03,
                                                maxPacketSize, d->bInterval);
                } else p->release();
            } else {
                LogOzzyKext("  failed to open pipe 0x%02x", address);
            }
        } else {
            LogOzzyKext("  no interface found for pipe 0x%02x", address);
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