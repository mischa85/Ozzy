#include "OzzyKext.h"
#include "KextBus.h"
#include "../../../engine/core/ozzy_engine.h"
#include "../../../engine/shared/ozzy_shared_data.h"
#include "../../../engine/shared/ozzy_log.h"
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
    // Selector 2 (in: rate): OzzyHAL changes the sample rate
    virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments* args,
                                    IOExternalMethodDispatch* dispatch, OSObject* target,
                                    void* reference) override {
        if (selector == OZZY_METHOD_MIDI_OUT_PENDING) {
            if (mOwner) mOwner->MidiOutPending();
            return kIOReturnSuccess;
        }
        if (selector == OZZY_METHOD_APPLY_SETTING) {
            if (!mOwner || args->scalarInputCount != 2) return kIOReturnBadArgument;
            if (args->scalarInput[0] > 0xFFFF || args->scalarInput[1] > 0xFF) return kIOReturnBadArgument;
            return mOwner->ApplySetting((uint32_t)args->scalarInput[0], (uint32_t)args->scalarInput[1]);
        }
        if (selector == OZZY_METHOD_SET_RATE) {
            if (!mOwner || args->scalarInputCount != 1 || args->scalarInput[0] > 0xFFFFFFFF) return kIOReturnBadArgument;
            return mOwner->SetRate((uint32_t)args->scalarInput[0]);
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
 * @size returns the allocation size for IOFree. */
static struct ozzy_engine* CreateEngine(uint16_t vid, uint16_t pid, size_t* size) {
    const struct ozzy_device* device = ozzy_device_lookup(vid, pid);
    if (!device) {
        ozzy_log("%04x:%04x is not in the device list", vid, pid);
        return nullptr;
    }
    ozzy_log("  device: %s%s", device->name, device->status == OZZY_STATUS_EXPERIMENTAL ? " (experimental)" : "");

    *size = ozzy_engine_size(device);
    void* raw = *size ? IOMalloc(*size) : nullptr;
    return ozzy_engine_init(raw, device);
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
    ozzy_log("start (build %s)", OZZY_GIT_VERSION);

    mHostDevice = OSDynamicCast(IOUSBHostDevice, provider);
    if (!mHostDevice || !mHostDevice->open(this)) {
        ozzy_log("open failed");
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
        // Get device strings from IORegistry
        GetRegistryString(mHostDevice, "USB Vendor Name", mSHM->manufacturer_name, 64);
        GetRegistryString(mHostDevice, "USB Product Name", mSHM->product_name, 64);
        GetRegistryString(mHostDevice, "USB Serial Number", mSHM->serial_number, 64);
        
        ozzy_log("found device: %s %s (S/N: %s)", mSHM->manufacturer_name, mSHM->product_name,
            mSHM->serial_number[0] ? mSHM->serial_number : "none");
        ozzy_log("  USB ID: %04x:%04x", desc->idVendor, desc->idProduct);
        ozzy_log("  speed: %s",
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

    ozzy_log("configuring hardware...");
    if (!ConfigureHardware()) {
        ozzy_log("configuration failed");
        return false;
    }

    mBus->SetPipePool(mPipes, kMaxPipes);

    ozzy_log("initializing engine...");
    ozzy_engine_attach(mEngine, mBus->Bus(), mSHM);
    if (mHostDevice) {
        // The connection and the build, for ozzy_info (the tools show them)
        switch (mHostDevice->getSpeed()) {
        case kUSBHostConnectionSpeedLow: ozzy_engine_set_usb_speed(mEngine, OZZY_USB_SPEED_LOW); break;
        case kUSBHostConnectionSpeedFull: ozzy_engine_set_usb_speed(mEngine, OZZY_USB_SPEED_FULL); break;
        case kUSBHostConnectionSpeedHigh: ozzy_engine_set_usb_speed(mEngine, OZZY_USB_SPEED_HIGH); break;
        default:
            // Super, SuperPlus and SuperPlusBy2 (newer SDKs) come after High
            if (mHostDevice->getSpeed() > kUSBHostConnectionSpeedHigh)
                ozzy_engine_set_usb_speed(mEngine, OZZY_USB_SPEED_SUPER);
            break;
        }
    }
    ozzy_engine_set_driver_version(mEngine, OZZY_GIT_VERSION);
    ozzy_engine_set_anchors(mEngine, true);   // OzzyHAL's zero timestamps
    
    if (!ozzy_engine_start(mEngine)) {
        ozzy_log("engine start failed");
        return false;
    }
    mBus->WatchReset();

    ozzy_log("driver ready");

    // OzzyHAL only attaches to devices with audio (the Xone:1D is MIDI only)
    setProperty(OZZY_AUDIO_PROPERTY_KEY, mSHM->audio.input_channels + mSHM->audio.output_channels > 0);
    mRunning = true;

    // Sleep and wake (setPowerState), as a child of the USB device in the power plane
    PMinit();
    provider->joinPMtree(this);
    mPMInited = registerPowerDriver(this, sPowerStates, kPowerStates) == kIOReturnSuccess;
    StartWatch();

    registerService();

    return true;
}

void OzzyKext::MidiOutPending() {
    if (mRunning && mEngine) ozzy_engine_midi_out_pending(mEngine);
}

/* OZZY_METHOD_APPLY_SETTING: restart the engine with setting @id = @value, the way the official
 * driver suspends streaming, sends the setting and resumes (ozzy_engine_restart). The same value
 * again restarts too: the handshake re-reads the digital lock. hardwarePresent and driverReady
 * stay set, so the HAL keeps the device; it calls this with IO stopped. */
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
    if (id >= OZZY_NUM_SETTINGS || ozzy_setting_descs[id].kind != OZZY_SETTING_DEVICE ||
        ozzy_engine_change_setting(mEngine, id, value) == OZZY_SETTING_INVALID)
        return kIOReturnBadArgument;
    return RestartLocked();
}

/* OZZY_METHOD_SET_RATE: restart the engine at @rate, if it doesn't run at it already */
IOReturn OzzyKext::SetRate(uint32_t rate) {
    if (!mSettingsLock) return kIOReturnNotReady;
    IOLockLock(mSettingsLock);
    IOReturn ret = kIOReturnNotReady;
    if (mRunning && mEngine) {
        enum ozzy_setting_change change = ozzy_engine_change_rate(mEngine, rate);
        ret = change == OZZY_SETTING_INVALID ? kIOReturnBadArgument
            : change == OZZY_SETTING_RESTART ? RestartLocked() : kIOReturnSuccess;
    }
    IOLockUnlock(mSettingsLock);
    return ret;
}

IOReturn OzzyKext::RestartLocked() {
    if (ozzy_engine_restart(mEngine)) {
        mBus->WatchReset();
        return kIOReturnSuccess;
    }
    // The device didn't take it: let the HAL drop it, as on a failed start
    mRunning = false;
    return kIOReturnIOError;
}

IOReturn OzzyKext::message(UInt32 type, IOService* provider, void* argument) {
    if (type == kIOMessageServiceIsTerminated) {
        ozzy_log("device disconnected");
        // Wait for a settings restart in progress (its requests fail fast once the device is gone)
        if (mSettingsLock) IOLockLock(mSettingsLock);
        mRunning = false;
        if (mSettingsLock) IOLockUnlock(mSettingsLock);
        
        // Immediately signal HAL that hardware is gone
        if (mSHM) {
            mSHM->audio.hardware_present = false;
            mSHM->audio.driver_ready = false;
        }
        
        // CRITICAL: Stop the engine, which aborts every transfer, NOW to prevent callback deadlocks
        if (mEngine) {
            ozzy_engine_stop(mEngine);
        }
        
        // Close the device and interfaces now: IOKit only calls stop() once we have closed
        // our provider, so closing them in stop() leaves the device and both interfaces busy
        // forever. After a sleep/wake that becomes a 60 s registry busy timeout panic (#52).
        CloseProviders();
    }
    
    // MUST call super - failure to do so causes termination timeout and kernel panic
    return IOService::message(type, provider, argument);
}

/* Close the interfaces and the device (ozzy_engine_stop has aborted every transfer). Safe to
 * call twice. */
void OzzyKext::CloseProviders() {
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
    ozzy_log("stop");
    
    // 0. Invalidate shared memory magic to signal HAL immediately
    if (mSHM) {
        mSHM->magic = 0;
    }
    
    // 1. No more recoveries or power changes, then stop the engine (clears flags, signals HAL,
    // aborts every transfer)
    StopWatch();
    if (mPMInited) { PMstop(); mPMInited = false; }
    mRunning = false;
    if (mEngine) { 
        ozzy_engine_stop(mEngine);
    }
    
    // 2. Close interfaces and device (if not done on termination)
    CloseProviders();
    
    // 3. Give USB stack time to complete callbacks
    IOSleep(100);
    
    // 4. Now safe to release pipes
    ReleasePipes();
    
    // 5. Release interfaces (closed in CloseProviders)
    for (int i = 0; i < kMaxInterfaces; i++) {
        if (mInterfaces[i]) { 
            mInterfaces[i]->release(); 
            mInterfaces[i] = nullptr; 
        }
    }
    
    // 6. Clean up engine and bus
    if (mEngine) { 
        IOFree(mEngine, mEngineSize);
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
    ozzy_log("free");
    if (mSettingsLock) {
        IOLockFree(mSettingsLock);
        mSettingsLock = nullptr;
    }
    IOService::free();
}

bool OzzyKext::MapSharedMemory() {
    /* Every USB transfer prepares and completes a slice of this buffer, and slices forward that
     * to it: from the completion thread calls and the timer at the same time. Without
     * kIOMemoryThreadSafe its wire count races, and complete() panics (#73). */
    mShmDescriptor = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIOMemoryPhysicallyContiguous | kIOMemoryThreadSafe, sizeof(ozzy_shared_memory), 0);
    
    if (!mShmDescriptor) return false;
    
    mShmDescriptor->prepare();
    mShmMap = mShmDescriptor->map();
    mSHM = (ozzy_shared_memory*)mShmMap->getVirtualAddress();
    memset(mSHM, 0, sizeof(ozzy_shared_memory));
    // The header (magic, version, USB IDs) comes with ozzy_engine_attach

    // Generate random session ID using current time and address
    uint32_t random = (uint32_t)((uintptr_t)mSHM ^ mach_absolute_time());
    mSHM->session_id = random;
    
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
    if (!wl) { ozzy_log("no workloop"); return false; }

    const struct ozzy_device_profile& profile = mEngine->profile;

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

            for(int i=0; i < profile.interface_count; i++) {
                if (d->bInterfaceNumber == profile.interfaces[i] && !mInterfaces[i]) {
                    if (iface->open(this)) {
                        iface->retain();
                        mInterfaces[i] = iface;
                        foundCount++;
                        if (profile.alt_settings[i]) {
                            iface->selectAlternateSetting(profile.alt_settings[i]);
                            IOSleep(50);
                        }
                    }
                }
            }
        }
        it->release();
    }

    if (foundCount != profile.interface_count) return false;

    // Every endpoint of the claimed interfaces to the engine, which picks its pipes
    struct ozzy_usb_endpoint eps[32];
    unsigned int n = 0;
    for (int i = 0; i < profile.interface_count; i++) {
        const StandardUSB::ConfigurationDescriptor* cfg = mInterfaces[i]->getConfigurationDescriptor();
        const StandardUSB::InterfaceDescriptor* intf = mInterfaces[i]->getInterfaceDescriptor();
        if (!cfg || !intf) continue;
        const StandardUSB::EndpointDescriptor* ep = nullptr;
        while (n < 32 && (ep = StandardUSB::getNextEndpointDescriptor(cfg, intf, ep))) {
            eps[n].intf = intf->bInterfaceNumber;
            eps[n].address = ep->bEndpointAddress;
            eps[n].attributes = ep->bmAttributes;
            eps[n].interval = ep->bInterval;
            eps[n].max_packet = USBToHost16(ep->wMaxPacketSize);
            eps[n++].handle = mInterfaces[i];
        }
    }
    const struct ozzy_usb_endpoint* bound[OZZY_PIPE_ROLES];
    ozzy_engine_bind_endpoints(mEngine, eps, n, bound);

    mPipeCount = 0;
    for (int k = 0; k < OZZY_PIPE_ROLES; k++) {
        if (!bound[k]) continue;
        int i = 0;
        while (i < profile.interface_count && mInterfaces[i] != bound[k]->handle) i++;
        if (mPipeCount >= kMaxPipes || i == profile.interface_count) return false;
        mPipeInterface[mPipeCount] = (uint8_t)i;
        mPipeAddress[mPipeCount++] = bound[k]->address;
    }
    return CopyPipes();
}

/* Open the pipes ConfigureHardware picked, on the interfaces' current alternate setting */
bool OzzyKext::CopyPipes() {
    for (int k = 0; k < mPipeCount; k++) {
        IOUSBHostInterface* iface = mInterfaces[mPipeInterface[k]];
        mPipes[k] = iface ? iface->copyPipe(mPipeAddress[k]) : nullptr;
        if (!mPipes[k]) {
            ozzy_log("  failed to open pipe 0x%02x", mPipeAddress[k]);
            return false;
        }
    }
    return true;
}

void OzzyKext::ReleasePipes() {
    for (int i = 0; i < kMaxPipes; i++) {
        if (mPipes[i]) {
            mPipes[i]->release();
            mPipes[i] = nullptr;
        }
    }
}

/* SET_INTERFACE @alt on @iface; a stall asked again, as the handshake's requests (up to 10 times,
 * 20 ms apart) */
static bool SelectAlternate(IOUSBHostInterface* iface, uint8_t alt) {
    IOReturn err = kIOReturnError;
    for (int attempt = 0; attempt < 10; attempt++) {
        if (attempt) IOSleep(20);
        err = iface->selectAlternateSetting(alt);
        if (err != kUSBHostReturnPipeStalled && err != kIOReturnNotResponding) break;
    }
    if (err != kIOReturnSuccess)
        ozzy_log("interface %u: alternate setting %u failed (0x%x)",
                 iface->getInterfaceDescriptor() ? iface->getInterfaceDescriptor()->bInterfaceNumber : 0, alt, err);
    return err == kIOReturnSuccess;
}

/* SET_INTERFACE 0, then @alts[i], on each interface i whose setting isn't 0 (ozzy_engine_reselect,
 * with the engine suspended: nothing in flight, nothing submitted); the setting even when 0
 * failed. The pipes belong to the old setting: the bus forgets them, they're released, and the
 * new setting's are opened. */
bool OzzyKext::Reselect(const uint8_t alts[4]) {
    if (!mBus) return false;
    mBus->SetPipePool(nullptr, 0);
    ReleasePipes();
    bool ok = true;
    for (int i = 0; i < kMaxInterfaces; i++) {
        if (!mInterfaces[i] || !alts[i]) continue;
        if (!SelectAlternate(mInterfaces[i], 0)) ok = false;
        if (!SelectAlternate(mInterfaces[i], alts[i])) ok = false;
        IOSleep(50);   // as after the first selection (ConfigureHardware)
    }
    if (!CopyPipes()) ok = false;
    mBus->SetPipePool(mPipes, kMaxPipes);
    return ok;
}

/* --- Stall watchdog --- */

static const uint32_t kWatchPeriodMs = 250;
static const uint32_t kWatchDeadMs = 500;        // ~100 transfer periods: no late completion
static const uint32_t kWatchRecoverS = 10;       // between recoveries: not forever on a dead device

static uint64_t MsToAbs(uint64_t ms) {
    uint64_t abs = 0;
    nanoseconds_to_absolutetime(ms * 1000000ULL, &abs);
    return abs;
}

void OzzyKext::StartWatch() {
    if (!mWatch) mWatch = thread_call_allocate_with_options(&OzzyKext::WatchFired, this, THREAD_CALL_PRIORITY_KERNEL, 0);
    if (!mWatch) return;
    mWatchOn = true;
    mWatchArmed = true;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    thread_call_enter_delayed(mWatch, mach_absolute_time() + MsToAbs(kWatchPeriodMs));
}

/* Wait until no call is queued or running (as KextBus::StopTimer), then free it */
void OzzyKext::StopWatch() {
    mWatchOn = false;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!mWatch) return;
    for (;;) {
        if (thread_call_cancel(mWatch)) mWatchArmed = false;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (!mWatchArmed && !mWatchBusy) break;
        IOSleep(1);
    }
    thread_call_free(mWatch);
    mWatch = nullptr;
}

void OzzyKext::WatchFired(thread_call_param_t param, thread_call_param_t) {
    OzzyKext* self = (OzzyKext*)param;
    self->mWatchBusy = true;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    self->mWatchArmed = false;
    if (self->mWatchOn) {
        IOLockLock(self->mSettingsLock);
        self->WatchLocked();
        IOLockUnlock(self->mSettingsLock);
    }
    if (self->mWatchOn) {
        self->mWatchArmed = true;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        thread_call_enter_delayed(self->mWatch, mach_absolute_time() + MsToAbs(kWatchPeriodMs));
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    self->mWatchBusy = false;
}

/* Under mSettingsLock, so never in the middle of a restart: a PCM pipe that stopped completing
 * gets the device a reselect and a new start, counted in the stats (ozzy-cli shows it) */
void OzzyKext::WatchLocked() {
    if (!mRunning || mSleeping || !mEngine || !mBus) return;
    static const enum ozzy_pipe_role roles[] = { OZZY_PIPE_PCM_OUT, OZZY_PIPE_PCM_IN };
    uint64_t now = mach_absolute_time();
    uint8_t dead = 0;
    for (unsigned i = 0; i < sizeof(roles) / sizeof(roles[0]) && !dead; i++) {
        uint8_t address = mEngine->pipe[roles[i]].address;
        if (address && mBus->Submitted(address) && now - mBus->LastCompletion(address) > MsToAbs(kWatchDeadMs))
            dead = address;
    }
    if (!dead || (mRecoveredAt && now - mRecoveredAt < MsToAbs(kWatchRecoverS * 1000ULL))) return;

    mRecoveredAt = now;
    ozzy_log("pipe 0x%02x: no completion for %u ms, recovering the device", dead,
             (unsigned)(kWatchDeadMs));
    ozzy_engine_suspend(mEngine);
    bool streaming = ozzy_engine_start(mEngine);   // a restart: it selects the interfaces again
    ozzy_engine_stats_recovery(mEngine, streaming);
    if (streaming) {
        mBus->WatchReset();
        ozzy_log("recovered: streaming");
    } else {
        // As a failed restart: the HAL drops the device
        ozzy_log("recovered: start failed");
        ozzy_engine_stop(mEngine);
        mRunning = false;
    }
}

/* --- Sleep and wake --- */

IOPMPowerState OzzyKext::sPowerStates[kPowerStates] = {
    { kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { kIOPMPowerStateVersion1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
};

/* Sleep: suspend (the HAL keeps the device). Wake: the transfers in flight across the sleep never
 * complete on some devices (the WZ4, #85), so the interfaces again and a new start. */
IOReturn OzzyKext::setPowerState(unsigned long state, IOService*) {
    if (!mSettingsLock) return kIOPMAckImplied;
    IOLockLock(mSettingsLock);
    if (state == 0 && mRunning && !mSleeping) {
        ozzy_log("sleep: suspending");
        ozzy_engine_suspend(mEngine);
        mSleeping = true;
    } else if (state != 0 && mSleeping) {
        mSleeping = false;
        if (mRunning) {
            ozzy_log("wake: starting again");
            if (ozzy_engine_start(mEngine)) {   // a restart: it selects the interfaces again
                mBus->WatchReset();
            } else {
                ozzy_log("start after wake failed");
                ozzy_engine_stop(mEngine);
                mRunning = false;
            }
        }
    }
    IOLockUnlock(mSettingsLock);
    return kIOPMAckImplied;
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