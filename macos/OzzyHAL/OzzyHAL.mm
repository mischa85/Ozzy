#include "OzzyHAL.h"
#include "../../common/devices/ploytec/ploytec_protocol.h"
#include "../Shared/OzzyLog.h"
#include "../Shared/OzzyKextClient.h"
#include "../../common/ozzy_version.h"
#include <CoreAudio/CoreAudio.h>

static CFStringRef SafeCreateString(const char* str) {
    if (!str || str[0] == 0) return nullptr;
    CFStringRef s = CFStringCreateWithCString(NULL, str, kCFStringEncodingUTF8);
    if (!s) s = CFStringCreateWithCString(NULL, str, kCFStringEncodingMacRoman);
    if (!s) s = CFStringCreateWithCString(NULL, str, kCFStringEncodingASCII);
    return s;
}

static UInt32 GetAudioBufferListSize(UInt32 numBuffers) {
    return (UInt32)(offsetof(AudioBufferList, mBuffers) + (numBuffers * sizeof(AudioBuffer)));
}

static bool IsOutputScope(AudioObjectPropertyScope scope) {
    return (scope == kAudioObjectPropertyScopeOutput || scope == kAudioObjectPropertyScopeGlobal);
}

// --- Write Helpers ---
static OSStatus WriteCFString(UInt32 inMax, UInt32* outSize, void* outData, CFStringRef s) {
    if (outData && inMax < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
    if (outData) { *(CFStringRef*)outData = s; CFRetain(s); }
    if (outSize) *outSize = sizeof(CFStringRef);
    return kAudioHardwareNoError;
}

static OSStatus WriteClassID(UInt32 inMax, UInt32* outSize, void* outData, AudioClassID v) {
    if (outData && inMax < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
    if (outData) *(AudioClassID*)outData = v;
    if (outSize) *outSize = sizeof(AudioClassID);
    return kAudioHardwareNoError;
}

static OSStatus WriteUInt32(UInt32 inMax, UInt32* outSize, void* outData, UInt32 v) {
    if (outData && inMax < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
    if (outData) *(UInt32*)outData = v;
    if (outSize) *outSize = sizeof(UInt32);
    return kAudioHardwareNoError;
}

static OSStatus WriteFloat64(UInt32 inMax, UInt32* outSize, void* outData, Float64 v) {
    if (outData && inMax < sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
    if (outData) *(Float64*)outData = v;
    if (outSize) *outSize = sizeof(Float64);
    return kAudioHardwareNoError;
}

static OSStatus WriteObjectID(UInt32 inMax, UInt32* outSize, void* outData, AudioObjectID v) {
    if (outData && inMax < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
    if (outData) *(AudioObjectID*)outData = v;
    if (outSize) *outSize = sizeof(AudioObjectID);
    return kAudioHardwareNoError;
}

// ============================================================================
// OzzyHALDevice
// ============================================================================

void OzzyHALDevice::Init(uint32_t slot) {
    mSlot = slot;

    mCurrentStreamFormat.mSampleRate = 96000.0;
    mCurrentStreamFormat.mFormatID = kAudioFormatLinearPCM;
    mCurrentStreamFormat.mFormatFlags = kAudioFormatFlagIsFloat;
    mCurrentStreamFormat.mBytesPerPacket = 4;     // per channel; GetStreamFormat() scales by channel count
    mCurrentStreamFormat.mFramesPerPacket = 1;
    mCurrentStreamFormat.mBytesPerFrame = 4;
    mCurrentStreamFormat.mChannelsPerFrame = 1;
    mCurrentStreamFormat.mBitsPerChannel = 32;
    mCurrentStreamFormat.mReserved = 0;

    ozzy_settings_defaults(&mSettings);
    UpdateRoutes();
    RebuildStreamConfigs();
}

/*
 * Pin the mapping for a call from a CoreAudio thread; null if there is none. Lock-free:
 * announce the use, then read the pointer. Unmap does the reverse (clear the pointer, then
 * wait for the users to go away), and with sequentially consistent ordering on both sides
 * either Unmap sees this user or this user sees the null pointer.
 */
ozzy_shared_memory* OzzyHALDevice::AcquireSHM() {
    mSHMUsers.fetch_add(1, std::memory_order_seq_cst);
    return mSHM.load(std::memory_order_seq_cst);
}

void OzzyHALDevice::ReleaseSHM() {
    mSHMUsers.fetch_sub(1, std::memory_order_release);
}

void OzzyHALDevice::ClearOutputBuffer() {
    ozzy_shared_memory* shm = AcquireSHM();
    if (shm && mClearOutput) mClearOutput(shm->audio.output_buffer, mOutRingFrames);
    ReleaseSHM();
}

void OzzyHALDevice::RebuildStreamConfigs() {
    const uint32_t kDefaultIOFrames = 512;

    mInputConfig.mNumberBuffers = 1;
    mInputConfig.mBuffers[0].mNumberChannels = mInputChannels;
    mInputConfig.mBuffers[0].mDataByteSize = kDefaultIOFrames * GetStreamFormat(false).mBytesPerFrame;
    mInputConfig.mBuffers[0].mData = nullptr;

    mOutputConfig.mNumberBuffers = 1;
    mOutputConfig.mBuffers[0].mNumberChannels = mOutputChannels;
    mOutputConfig.mBuffers[0].mDataByteSize = kDefaultIOFrames * GetStreamFormat(true).mBytesPerFrame;
    mOutputConfig.mBuffers[0].mData = nullptr;
}

/* A UID that stays the same across replugs, so CoreAudio remembers the device's settings.
 * Ploytec devices have no real serial number: the DB2 and DB4 return the string "no serial
 * number" (captures in captures/), the Xone:1D has none. Those get model + USB port, like
 * macOS does for other serial-less USB audio devices; moving the device to another port
 * makes it a new device to CoreAudio. */
void OzzyHALDevice::MakeDeviceUID(const ozzy_shared_memory* shm, uint32_t locationID, char* buf, size_t size) {
    bool hasSerial = shm->serial_number[0] != 0 && strcmp(shm->serial_number, "no serial number") != 0;
    if (hasSerial) snprintf(buf, size, "OzzyHAL.%s", shm->serial_number);
    else snprintf(buf, size, "OzzyHAL.%04X:%04X.%08X", shm->vendor_id, shm->product_id, locationID);
}

bool OzzyHALDevice::HasDeviceUID(const char* uid) const {
    if (!mDeviceUID) return false;
    char buf[128];
    return CFStringGetCString(mDeviceUID, buf, sizeof(buf), kCFStringEncodingUTF8) && strcmp(buf, uid) == 0;
}

/* Take over a mapped kext connection. Returns false (and leaves it to the caller to close)
 * if the shared memory isn't usable. */
bool OzzyHALDevice::Map(io_connect_t connect, mach_vm_address_t address, mach_vm_size_t size, uint64_t registryID, uint32_t locationID) {
    ozzy_shared_memory* shm = (ozzy_shared_memory*)address;
    if (shm->magic != OZZY_MAGIC) {
        LogOzzyHALError("SHM magic mismatch");
        return false;
    }
    if (shm->version != OZZY_SHARED_VERSION) {
        LogOzzyHALError("SHM version %{public}u, expected %{public}u: reinstall the kext and HAL together",
                        shm->version, OZZY_SHARED_VERSION);
        return false;
    }
    if (shm->vendor_id == 0) return false;

    mKextConnect = connect;
    mKextMapAddress = address;
    mKextMapSize = size;
    mRegistryID = registryID;
    mLocationID = locationID;
    mSessionID = shm->session_id;
    mSHM.store(shm, std::memory_order_release);
    LogOzzyHAL("device %{public}u: attached to kext (session: 0x%{public}08X)", mSlot, mSessionID);
    return true;
}

/* Monitor thread. Waits for CoreAudio threads still using the mapping (at most one I/O
 * cycle) before unmapping, so they never touch unmapped memory. */
void OzzyHALDevice::Unmap() {
    if (!mSHM.exchange(nullptr, std::memory_order_seq_cst)) return;
    while (mSHMUsers.load(std::memory_order_seq_cst)) usleep(100);
    IOConnectUnmapMemory(mKextConnect, 0, mach_task_self(), mKextMapAddress);
    IOServiceClose(mKextConnect);
    mKextConnect = IO_OBJECT_NULL;
    mKextMapAddress = 0;
    mKextMapSize = 0;
    mRegistryID = 0;
}

/* Unmap if the kext went away or restarted. Returns true while still mapped. */
bool OzzyHALDevice::CheckHealth() {
    ozzy_shared_memory* shm = mSHM.load(std::memory_order_relaxed);
    if (!shm) return false;
    bool magicValid = (shm->magic == OZZY_MAGIC) && (shm->session_id == mSessionID);
    bool hwPresent = magicValid && shm->audio.hardware_present.load(std::memory_order_acquire);
    if (magicValid && hwPresent) return true;
    LogOzzyHAL("device %{public}u: disconnected (magic: %{public}d, hw: %{public}d)", mSlot, magicValid, hwPresent);
    Unmap();
    return false;
}

/* Select codec and sample rate from what the engine published in shared memory.
 * Called when the device comes up, because the engine only writes deviceFlags and
 * sampleRate during its Start() handshake. */
void OzzyHALDevice::ApplyDeviceConfig() {
    ozzy_shared_memory* shm = mSHM.load(std::memory_order_relaxed);
    if (!shm) return;

    // The rings as the engine laid them out for its transfers this run, and its clock spacing
    mOutRingFrames = shm->audio.output_ring_frames;
    mInRingFrames = shm->audio.input_ring_frames;
    mTimestampFrames = shm->audio.timestamp_frames;
    if (!mOutRingFrames || !mInRingFrames || !mTimestampFrames) {
        LogOzzyHALError("device %{public}u: the driver published no ring layout (an older kext?)", mSlot);
        mOutRingFrames = mOutRingFrames ? mOutRingFrames : 1;
        mInRingFrames = mInRingFrames ? mInRingFrames : 1;
        mTimestampFrames = mTimestampFrames ? mTimestampFrames : OZZY_TIMESTAMP_FRAMES;
    }
    // The engine reads output this far ahead (its OUT transfers in flight); input is complete
    // this far behind the clock (its IN transfers)
    mSafetyOffset = shm->audio.safety_offset_frames;
    mInputSafetyOffset = shm->audio.input_safety_offset_frames ? shm->audio.input_safety_offset_frames : mSafetyOffset;

    // Channel counts per direction, as published by the engine for this model
    mInputChannels = shm->audio.input_channels;
    mOutputChannels = shm->audio.output_channels;

    // Ploytec bulk layout: 512-byte output packets (follows wMaxPacketSize, not transfer type).
    // Plain output: a ring of interleaved S24 frames that isochronous transfers read as is.
    // Writer and reader are picked here once; the I/O path doesn't branch on layout or channels.
    uint32_t flags = shm->audio.device_flags;
    bool isBulk = flags & OZZY_DEVICE_FLAG_PLOYTEC_BULK;
    bool plain = flags & OZZY_DEVICE_FLAG_PLAIN_OUTPUT;
    bool mirror = flags & OZZY_DEVICE_FLAG_SPDIF_MIRROR;
    mSpdifMirror = mirror && isBulk && !plain && mOutputChannels == 8;
    mWriteOutput = plain ? PloytecPlainOutputWriter(mOutputChannels) : PloytecOutputWriter(isBulk, mOutputChannels, mirror);
    mReadInput = PloytecInputReader(mInputChannels);
    if (!mWriteOutput || !mReadInput) {
        LogOzzyHALError("no codec for %{public}u in / %{public}u out, falling back to 8 / 8",
                        mInputChannels, mOutputChannels);
        mInputChannels = mOutputChannels = 8;
        mWriteOutput = plain ? PloytecPlainOutputWriter(8) : PloytecOutputWriter(isBulk, 8);
        mReadInput = PloytecInputReader(8);
    }
    mClearOutput = plain ? PloytecClearOutputPlain : isBulk ? PloytecClearOutputBulk : PloytecClearOutputInterrupt;
    RebuildStreamConfigs();
    LogOzzyHAL("device %{public}u: Ploytec codec (%{public}s output, %{public}u in / %{public}u out) for VID:0x%{public}04X",
               mSlot, plain ? "plain" : isBulk ? "bulk" : "interrupt", mInputChannels, mOutputChannels, shm->vendor_id);

    uint32_t rate = shm->audio.sample_rate.load();
    if (rate == 0) rate = 96000;
    mCurrentStreamFormat.mSampleRate = rate;

    // Device settings as the kext applied them, and the clock status of its last handshake.
    // Routes are the HAL's own (it routes in its codecs); they stay as they are.
    mSettingsAvailable = shm->audio.settings.available;
    for (unsigned int i = 0; i < OZZY_NUM_SETTINGS; i++) {
        if (ozzy_setting_descs[i].kind != OZZY_SETTING_DEVICE) continue;
        uint8_t v = shm->audio.settings.value[i];
        mSettings.value[i] = v < SettingCount(i) ? v : ozzy_setting_descs[i].def;
    }
    UpdateRoutes();
    mClockLocked = shm->audio.settings.clock_locked;
    mLockRate = shm->audio.settings.lock_rate;
    {
        std::lock_guard<std::mutex> lock(mInfoLock);
        memcpy(&mInfo, (const void*)&shm->info, sizeof(mInfo));
    }

    // The rates CoreAudio may pick: the device's, or only the S/PDIF lock rate
    const struct ozzy_device* model = ozzy_device_lookup(shm->vendor_id, shm->product_id);
    mHostRateCount = model ? ozzy_device_host_rates(model, mClockLocked ? mLockRate : 0, mHostRates) : 0;
    if (!mHostRateCount) {
        mHostRates[0] = rate;
        mHostRateCount = 1;
    }
    LogOzzyHAL("device %{public}u: sample rate %{public}u Hz (%{public}u to pick from)", mSlot, rate, mHostRateCount);
    if (HasSetting(OZZY_SETTING_CLOCK_SOURCE))
        LogOzzyHAL("device %{public}u: clock source %{public}s%{public}s", mSlot,
                   ozzy_clock_source_names[mSettings.value[OZZY_SETTING_CLOCK_SOURCE]],
                   mSettings.value[OZZY_SETTING_CLOCK_SOURCE] == OZZY_CLOCK_INTERNAL ? "" :
                   mClockLocked ? " (locked to S/PDIF)" : " (no S/PDIF lock: internal)");
}

/* OZZY_METHOD_APPLY_SETTING: the kext restarts its engine with the setting, which takes up to
 * about a second (the handshake). Call with the mapping held: AcquireSHM, or from the monitor
 * thread, which owns it. */
kern_return_t OzzyHALDevice::CallApplySetting(unsigned int id, uint32_t value) {
    uint64_t in[2] = { id, value };
    kern_return_t kr = IOConnectCallScalarMethod(mKextConnect, OZZY_METHOD_APPLY_SETTING, in, 2, nullptr, nullptr);
    if (kr != KERN_SUCCESS)
        LogOzzyHALError("device %{public}u: %{public}s %{public}s failed (0x%{public}x)", mSlot,
                        ozzy_setting_descs[id].name, ozzy_setting_descs[id].value_names[value], kr);
    return kr;   // ApplyDeviceConfig logs the result
}

/* Values of setting @id on this device (routes: the pairs on their source side) */
unsigned int OzzyHALDevice::SettingCount(unsigned int id) const {
    return HasSetting(id) ? ozzy_setting_count(id, mInputChannels, mOutputChannels) : 0;
}

/* The codecs' routing from the route settings, and the ESU1808's mirror writer for its S/PDIF
 * mirror setting. The IO thread reads them without a lock: a cycle during a change may mix the
 * old and new routing. */
void OzzyHALDevice::UpdateRoutes() {
    ozzy_settings_route(&mInRoute, &mSettings, mSettingsAvailable, true, mInputChannels);
    ozzy_settings_route(&mOutRoute, &mSettings, mSettingsAvailable, false, mOutputChannels);
    if (mSpdifMirror)
        mWriteOutput = PloytecOutputWriter(true, 8, true, mSettings.value[OZZY_SETTING_SPDIF_MIRROR]);
}

/* Where CoreAudio's plug-in storage keeps a setting: per device UID, so it follows the device */
CFStringRef OzzyHALDevice::CopySettingKey(unsigned int id) const {
    return CFStringCreateWithFormat(NULL, NULL, CFSTR("%@ %s"), mDeviceUID, ozzy_setting_descs[id].name);
}

void OzzyHALDevice::StoreSetting(AudioServerPlugInHostRef host, unsigned int id, uint32_t value) {
    CFStringRef key = CopySettingKey(id);
    CFNumberRef number = CFNumberCreate(NULL, kCFNumberSInt32Type, &value);
    if (host && key && number) host->WriteToStorage(host, key, number);
    if (number) CFRelease(number);
    if (key) CFRelease(key);
}

/* Monitor thread, before the device is published: apply the stored settings. Routes are the
 * HAL's; device settings go to the kext when it doesn't have them yet (it starts every device
 * with the defaults). */
void OzzyHALDevice::RestoreSettings(AudioServerPlugInHostRef host) {
    if (!host || !mDeviceUID) return;
    bool changed = false;
    for (unsigned int id = 0; id < OZZY_NUM_SETTINGS; id++) {
        if (!HasSetting(id)) continue;
        CFStringRef key = CopySettingKey(id);
        CFPropertyListRef stored = nullptr;
        if (key && host->CopyFromStorage(host, key, &stored) == kAudioHardwareNoError && stored) {
            UInt32 value = 0;
            if (CFGetTypeID(stored) == CFNumberGetTypeID() &&
                CFNumberGetValue((CFNumberRef)stored, kCFNumberSInt32Type, &value) &&
                value < SettingCount(id) && value != mSettings.value[id]) {
                if (ozzy_setting_descs[id].kind != OZZY_SETTING_DEVICE) mSettings.value[id] = (uint8_t)value;
                else changed |= CallApplySetting(id, value) == KERN_SUCCESS;
            }
            CFRelease(stored);
        }
        if (key) CFRelease(key);
    }
    if (changed) ApplyDeviceConfig();
    else UpdateRoutes();
}

/* OZZY_PROPERTY_SETTING: every setting the device has by name, and the clock status */
CFDictionaryRef OzzyHALDevice::CopySettingsDictionary() const {
    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                            &kCFTypeDictionaryValueCallBacks);
    if (!dict) return nullptr;
    for (unsigned int id = 0; id < OZZY_NUM_SETTINGS; id++) {
        if (!HasSetting(id)) continue;
        CFStringRef key = CFStringCreateWithCString(NULL, ozzy_setting_descs[id].name, kCFStringEncodingUTF8);
        SInt32 v = mSettings.value[id];
        CFNumberRef number = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
        if (key && number) CFDictionarySetValue(dict, key, number);
        if (key) CFRelease(key);
        if (number) CFRelease(number);
    }
    if (HasSetting(OZZY_SETTING_CLOCK_SOURCE)) {
        SInt32 rate = (SInt32)mLockRate;
        CFNumberRef number = CFNumberCreate(NULL, kCFNumberSInt32Type, &rate);
        CFDictionarySetValue(dict, CFSTR("Clock Locked"), mClockLocked ? kCFBooleanTrue : kCFBooleanFalse);
        if (number) { CFDictionarySetValue(dict, CFSTR("Lock Rate"), number); CFRelease(number); }
    }
    return dict;
}

/* OZZY_PROPERTY_INFO: the kext's record as it is (ozzy-cli and the ozzy app decode it) */
CFDataRef OzzyHALDevice::CopyInfoData() const {
    std::lock_guard<std::mutex> lock(mInfoLock);
    return CFDataCreate(NULL, (const UInt8*)&mInfo, sizeof(mInfo));
}

/* OZZY_PROPERTY_STATS: the kext's counters as they are now (zeros while unmapped) */
CFDataRef OzzyHALDevice::CopyStatsData() {
    struct ozzy_stats stats = {};
    ozzy_shared_memory* shm = AcquireSHM();
    if (shm) memcpy(&stats, (const void*)&shm->stats, sizeof(stats));
    ReleaseSHM();
    return CFDataCreate(NULL, (const UInt8*)&stats, sizeof(stats));
}

bool OzzyHALDevice::StatsChanged() {
    ozzy_shared_memory* shm = AcquireSHM();
    uint32_t incidents = shm ? ozzy_stats_incidents(&shm->stats) : 0;
    ReleaseSHM();
    if (incidents == mStatsIncidents) return false;
    mStatsIncidents = incidents;
    return true;
}

/* OZZY_PROPERTY_SETTING set: each name -> value in @dict. Returns the first error, after trying all. */
OSStatus OzzyHALDevice::SetSettingsDictionary(AudioServerPlugInHostRef host, CFDictionaryRef dict) {
    if (!dict || CFGetTypeID(dict) != CFDictionaryGetTypeID()) return kAudioHardwareIllegalOperationError;
    OSStatus result = kAudioHardwareNoError;
    for (unsigned int id = 0; id < OZZY_NUM_SETTINGS; id++) {
        CFStringRef key = CFStringCreateWithCString(NULL, ozzy_setting_descs[id].name, kCFStringEncodingUTF8);
        CFTypeRef value = key ? CFDictionaryGetValue(dict, key) : nullptr;
        if (key) CFRelease(key);
        if (!value) continue;
        SInt32 v = -1;
        OSStatus st = (CFGetTypeID(value) == CFNumberGetTypeID() && CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, &v) &&
                       v >= 0 && HasSetting(id))
                    ? SetSetting(host, id, (UInt32)v) : kAudioHardwareIllegalOperationError;
        if (result == kAudioHardwareNoError) result = st;
    }
    return result;
}

/* The name of a clock source item; the selected Automatic item says which clock the device
 * runs on. Returns a new string. */
CFStringRef OzzyHALDevice::CopyClockSourceName(UInt32 item) const {
    if (item != OZZY_CLOCK_AUTOMATIC || mSettings.value[OZZY_SETTING_CLOCK_SOURCE] != OZZY_CLOCK_AUTOMATIC)
        return CFStringCreateWithCString(NULL, ozzy_clock_source_names[item], kCFStringEncodingUTF8);
    if (mClockLocked)
        return CFStringCreateWithFormat(NULL, NULL, CFSTR("%s (S/PDIF, %u Hz)"), ozzy_clock_source_names[item], mLockRate);
    return CFStringCreateWithFormat(NULL, NULL, CFSTR("%s (internal, no S/PDIF signal)"), ozzy_clock_source_names[item]);
}

/* A client set a setting. A route applies at once. A device setting restarts the device's
 * streaming (and may change the rate), so CoreAudio stops IO first and then calls
 * PerformSettingChange; the change action carries the setting and its value. */
OSStatus OzzyHALDevice::SetSetting(AudioServerPlugInHostRef host, unsigned int id, UInt32 value) {
    if (!host || value >= SettingCount(id)) return kAudioHardwareIllegalOperationError;
    if (value == mSettings.value[id]) return kAudioHardwareNoError;
    if (ozzy_setting_descs[id].kind == OZZY_SETTING_DEVICE)
        return host->RequestDeviceConfigurationChange(host, GetDeviceID(), ((UInt64)id << 32) | value, nullptr);

    mSettings.value[id] = (uint8_t)value;
    UpdateRoutes();
    StoreSetting(host, id, value);
    LogOzzyHAL("device %{public}u: %{public}s %{public}s", mSlot, ozzy_setting_descs[id].name,
               ozzy_setting_descs[id].value_names[value]);
    AudioObjectPropertyAddress addr = { OZZY_PROPERTY_SETTING, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    host->PropertiesChanged(host, GetDeviceID(), 1, &addr);
    return kAudioHardwareNoError;
}

/* The change action of a rate change; a setting's is (id << 32) | value */
static const UInt64 kRateChangeAction = 1ULL << 63;

OSStatus OzzyHALDevice::SetSampleRate(AudioServerPlugInHostRef host, Float64 rate) {
    uint32_t hz = (uint32_t)rate;
    if (!host) return kAudioHardwareIllegalOperationError;
    if (hz == (uint32_t)mCurrentStreamFormat.mSampleRate) return kAudioHardwareNoError;
    for (uint32_t i = 0; i < mHostRateCount; i++) {
        if (mHostRates[i] == hz)
            return host->RequestDeviceConfigurationChange(host, GetDeviceID(), kRateChangeAction | hz, nullptr);
    }
    return kAudioDeviceUnsupportedFormatError;
}

/* OZZY_METHOD_SET_RATE: the kext restarts its engine at the rate (about a second, the
 * handshake). Call with the mapping held. */
kern_return_t OzzyHALDevice::CallSetRate(uint32_t rate) {
    uint64_t in[1] = { rate };
    kern_return_t kr = IOConnectCallScalarMethod(mKextConnect, OZZY_METHOD_SET_RATE, in, 1, nullptr, nullptr);
    if (kr != KERN_SUCCESS)
        LogOzzyHALError("device %{public}u: %{public}u Hz failed (0x%{public}x)", mSlot, rate, kr);
    return kr;   // ApplyDeviceConfig logs the result
}

OSStatus OzzyHALDevice::PerformConfigurationChange(AudioServerPlugInHostRef host, UInt64 action) {
    bool rateChange = action & kRateChangeAction;
    unsigned int id = (unsigned int)(action >> 32);
    uint32_t value = (uint32_t)action;
    if (!rateChange && (value >= SettingCount(id) || ozzy_setting_descs[id].kind != OZZY_SETTING_DEVICE))
        return kAudioHardwareIllegalOperationError;

    ozzy_shared_memory* shm = AcquireSHM();
    kern_return_t kr = !shm ? kIOReturnNotAttached : rateChange ? CallSetRate(value) : CallApplySetting(id, value);
    if (shm) ApplyDeviceConfig();
    ReleaseSHM();

    if (kr == KERN_SUCCESS && !rateChange) StoreSetting(host, id, value);
    // The rate may have changed (S/PDIF lock), and the clock source item names say the lock
    if (host) NotifyDeviceChanged(host);
    return kr == KERN_SUCCESS ? kAudioHardwareNoError : kAudioHardwareUnspecifiedError;
}

void OzzyHALDevice::UpdateMetadata() {
    ozzy_shared_memory* shm = mSHM.load(std::memory_order_relaxed);
    if (shm->product_name[0] != 0) {
        if (mModelUID) CFRelease(mModelUID);
        mModelUID = SafeCreateString(shm->product_name);
    }
    if (shm->manufacturer_name[0] != 0) {
        if (mManufacturerUID) CFRelease(mManufacturerUID);
        mManufacturerUID = SafeCreateString(shm->manufacturer_name);
    }
    char uid[128];
    MakeDeviceUID(shm, mLocationID, uid, sizeof(uid));
    if (mDeviceUID) CFRelease(mDeviceUID);
    mDeviceUID = SafeCreateString(uid);
    if (!mModelUID) mModelUID = CFSTR("Ozzy Audio Device");
    if (!mManufacturerUID) mManufacturerUID = CFSTR("Ozzy");
}

void OzzyHALDevice::NotifyDeviceChanged(AudioServerPlugInHostRef host) {
    AudioObjectID dev = GetDeviceID();
    if (mConnected) {
        // Device connected - notify about device properties first; the caller updates the list
        AudioObjectPropertyAddress devAddr[5] = {
            { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
            { kAudioObjectPropertyManufacturer, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
            { kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
            { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
            { kAudioDevicePropertyAvailableNominalSampleRates, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain } };
        host->PropertiesChanged(host, dev, 5, devAddr);
        // Channel counts follow the model (e.g. WZ4: 16 in / 2 out)
        AudioObjectPropertyAddress cfgAddr[2] = {
            { kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain },
            { kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain } };
        host->PropertiesChanged(host, dev, 2, cfgAddr);
        // The transfer sizes (device settings) set the safety offsets and the clock spacing
        AudioObjectPropertyAddress timingAddr[3] = {
            { kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain },
            { kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain },
            { kAudioDevicePropertyZeroTimeStampPeriod, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain } };
        host->PropertiesChanged(host, dev, 3, timingAddr);
        AudioObjectPropertyAddress fmtAddr[2] = {
            { kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
            { kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain } };
        host->PropertiesChanged(host, GetInputStreamID(), 2, fmtAddr);
        host->PropertiesChanged(host, GetOutputStreamID(), 2, fmtAddr);
        AudioObjectPropertyAddress infoAddr = { OZZY_PROPERTY_INFO, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        host->PropertiesChanged(host, dev, 1, &infoAddr);
        if (mSettingsAvailable) {
            AudioObjectPropertyAddress setAddr = { OZZY_PROPERTY_SETTING, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
            host->PropertiesChanged(host, dev, 1, &setAddr);
        }
        if (HasSetting(OZZY_SETTING_CLOCK_SOURCE)) {
            AudioObjectPropertyAddress ctlAddr[2] = {
                { kAudioObjectPropertyControlList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
                { kAudioObjectPropertyOwnedObjects, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain } };
            host->PropertiesChanged(host, dev, 2, ctlAddr);
            AudioObjectPropertyAddress clkAddr[3] = {
                { kAudioSelectorControlPropertyCurrentItem, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
                { kAudioSelectorControlPropertyAvailableItems, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
                { kAudioSelectorControlPropertyItemName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain } };
            host->PropertiesChanged(host, GetClockControlID(), 3, clkAddr);
        }
    } else {
        // Device disconnected - notify that it is no longer alive; the caller updates the list
        AudioObjectPropertyAddress aliveAddr = { kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        host->PropertiesChanged(host, dev, 1, &aliveAddr);
    }
}

/* Publish the device once the engine is up, withdraw it when it goes down */
bool OzzyHALDevice::UpdateConnectionState(AudioServerPlugInHostRef host) {
    ozzy_shared_memory* shm = mSHM.load(std::memory_order_relaxed);
    bool hwReady = (shm && shm->audio.hardware_present.load(std::memory_order_acquire));
    bool drvReady = (shm && shm->audio.driver_ready.load(std::memory_order_acquire));
    bool metaReady = (shm && shm->product_name[0] != 0);
    bool currentlyConnected = (hwReady && drvReady && metaReady);
    if (currentlyConnected == mConnected) return false;

    if (currentlyConnected) {
        UpdateMetadata();
        ozzy_settings_defaults(&mSettings);   // routes start at their defaults; storage may change them
        ApplyDeviceConfig();
        RestoreSettings(host);
        LogOzzyHAL("device %{public}u ready: '%{public}s'", mSlot, shm->product_name);
    } else if (mIOStarted) {
        mIOStarted = false;
        mTimestampSeed++;
        LogOzzyHAL("device %{public}u: auto-stopped I/O due to disconnect", mSlot);
        std::lock_guard<std::mutex> lock(mClientLock);
        for (int k = 0; k < mClientCount; k++) mClients[k].io = false;
        mIOCount = 0;
    }

    mConnected = currentlyConnected;
    LogOzzyHAL("device %{public}u: connection %{public}s", mSlot, currentlyConnected ? "UP" : "DOWN");
    if (host) NotifyDeviceChanged(host);
    return true;
}

void OzzyHALDevice::AddClient(const AudioServerPlugInClientInfo* info) {
    if (!info) return;
    std::lock_guard<std::mutex> lock(mClientLock);
    for (int i = 0; i < mClientCount; i++)
        if (mClients[i].id == info->mClientID) return;
    if (mClientCount == kMaxClients) return;
    Client* c = &mClients[mClientCount++];
    memset(c, 0, sizeof(*c));
    c->id = info->mClientID;
    c->pid = info->mProcessID;
    if (!info->mBundleID || !CFStringGetCString(info->mBundleID, c->bundle, sizeof(c->bundle), kCFStringEncodingUTF8))
        c->bundle[0] = 0;
    LogOzzyHAL("device %{public}u: client %{public}u added (pid %{public}d)", mSlot, c->id, (int)c->pid);
}

void OzzyHALDevice::RemoveClient(UInt32 clientID) {
    std::lock_guard<std::mutex> lock(mClientLock);
    for (int i = 0; i < mClientCount; i++) {
        if (mClients[i].id != clientID) continue;
        mClients[i] = mClients[--mClientCount];
        LogOzzyHAL("device %{public}u: client %{public}u removed", mSlot, clientID);
        return;
    }
}

void OzzyHALDevice::NotifyClients(AudioServerPlugInHostRef host) {
    if (!host) return;
    AudioObjectPropertyAddress a = { OZZY_PROPERTY_CLIENTS, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    host->PropertiesChanged(host, GetDeviceID(), 1, &a);
}

/* A client starts IO; the device's IO starts with the first */
kern_return_t OzzyHALDevice::StartIO(AudioServerPlugInHostRef host, UInt32 clientID) {
    bool first;
    int count;
    {
        std::lock_guard<std::mutex> lock(mClientLock);
        first = mIOCount++ == 0;
        count = mIOCount;
        for (int k = 0; k < mClientCount; k++)
            if (mClients[k].id == clientID) mClients[k].io = true;
    }
    LogOzzyHAL("device %{public}u: client %{public}u starts IO (%{public}d started)", mSlot, clientID, count);
    kern_return_t result = first ? StartDeviceIO() : kIOReturnSuccess;
    if (result != kIOReturnSuccess) {
        std::lock_guard<std::mutex> lock(mClientLock);
        mIOCount--;
        for (int k = 0; k < mClientCount; k++)
            if (mClients[k].id == clientID) mClients[k].io = false;
        return result;
    }
    NotifyClients(host);
    return result;
}

/* A client stops IO; the device's IO stops with the last */
kern_return_t OzzyHALDevice::StopIO(AudioServerPlugInHostRef host, UInt32 clientID) {
    bool last = false;
    int count;
    {
        std::lock_guard<std::mutex> lock(mClientLock);
        if (mIOCount > 0) last = --mIOCount == 0;
        count = mIOCount;
        for (int k = 0; k < mClientCount; k++)
            if (mClients[k].id == clientID || last) mClients[k].io = false;
    }
    LogOzzyHAL("device %{public}u: client %{public}u stops IO (%{public}d started)", mSlot, clientID, count);
    if (last) StopDeviceIO();
    NotifyClients(host);
    return kIOReturnSuccess;
}

CFArrayRef OzzyHALDevice::CopyClientsArray() const {
    CFMutableArrayRef array = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    std::lock_guard<std::mutex> lock(mClientLock);
    for (int i = 0; array && i < mClientCount; i++) {
        CFMutableDictionaryRef dict = CFDictionaryCreateMutable(NULL, 3, &kCFTypeDictionaryKeyCallBacks,
                                                                &kCFTypeDictionaryValueCallBacks);
        if (!dict) continue;
        SInt32 pid = mClients[i].pid;
        CFNumberRef number = CFNumberCreate(NULL, kCFNumberSInt32Type, &pid);
        if (number) { CFDictionarySetValue(dict, CFSTR("pid"), number); CFRelease(number); }
        CFDictionarySetValue(dict, CFSTR("io"), mClients[i].io ? kCFBooleanTrue : kCFBooleanFalse);
        if (mClients[i].bundle[0]) {
            CFStringRef bundle = CFStringCreateWithCString(NULL, mClients[i].bundle, kCFStringEncodingUTF8);
            if (bundle) { CFDictionarySetValue(dict, CFSTR("bundle"), bundle); CFRelease(bundle); }
        }
        CFArrayAppendValue(array, dict);
        CFRelease(dict);
    }
    return array;
}

kern_return_t OzzyHALDevice::StartDeviceIO() {
    if (!mConnected) return kAudioHardwareNotRunningError;
    ozzy_shared_memory* shm = AcquireSHM();
    if (shm) shm->audio.hal_write_position.store(0, std::memory_order_relaxed);
    ReleaseSHM();
    LogOzzyHAL("device %{public}u: start I/O", mSlot);
    mIOStarted = true;
    mTimestampSeed++;
    return kIOReturnSuccess;
}

void OzzyHALDevice::StopDeviceIO() {
    LogOzzyHAL("device %{public}u: stop I/O", mSlot);
    mIOStarted = false;
    mTimestampSeed++;
    ClearOutputBuffer();
}

HRESULT OzzyHALDevice::GetZeroTimeStamp(Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed) {
    ozzy_shared_memory* shm = AcquireSHM();
    if (!shm) {
        ReleaseSHM();
        return kAudioHardwareNotRunningError;
    }

    uint64_t sTime = 0, hTime = 0;
    ozzy_timestamp_read(&shm->audio.timestamp, &sTime, &hTime);
    ReleaseSHM();

    if (outSampleTime) *outSampleTime = (Float64)sTime;
    if (outHostTime) *outHostTime = hTime;
    if (outSeed) *outSeed = mTimestampSeed;
    
    return kAudioHardwareNoError;
}

OSStatus OzzyHALDevice::ioOperation(UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer) {
    ozzy_shared_memory* shm = AcquireSHM();
    if (!shm) {
        ReleaseSHM();
        return kAudioHardwareNoError;
    }

    if (inOperationID == kAudioServerPlugInIOOperationWriteMix) {
        if (mWriteOutput) {
            uint64_t sampleTime = (uint64_t)inIOCycleInfo->mOutputTime.mSampleTime;
            // Output routing in place: the mix buffer isn't used after this (in-place IO)
            ozzy_route_frames((uint8_t*)ioMainBuffer, inIOBufferFrameSize, &mOutRoute, 2 * sizeof(float));
            mWriteOutput(shm->audio.output_buffer, (const float*)ioMainBuffer, sampleTime, inIOBufferFrameSize, mOutRingFrames);
            shm->audio.hal_write_position.store(sampleTime + inIOBufferFrameSize, std::memory_order_release);
        }
    }
    else if (inOperationID == kAudioServerPlugInIOOperationReadInput) {
        if (mReadInput) {
            uint64_t sampleTime = (uint64_t)inIOCycleInfo->mInputTime.mSampleTime;
            mReadInput((float*)ioMainBuffer, shm->audio.input_buffer, sampleTime, inIOBufferFrameSize, mInRingFrames);
            ozzy_route_frames((uint8_t*)ioMainBuffer, inIOBufferFrameSize, &mInRoute, 2 * sizeof(float));
        }
    }
    ReleaseSHM();
    return kAudioHardwareNoError;
}

CFStringRef OzzyHALDevice::GetDeviceName() const { return mModelUID; }
CFStringRef OzzyHALDevice::GetManufacturer() const { return mManufacturerUID; }
CFStringRef OzzyHALDevice::GetDeviceUID() const { return mDeviceUID; }
CFStringRef OzzyHALDevice::GetModelUID() const { return mModelUID; }
UInt32 OzzyHALDevice::GetTransportType() const { return kAudioDeviceTransportTypeUSB; }
// Every device runs on its own clock, so CoreAudio must not treat two as synchronous
UInt32 OzzyHALDevice::GetClockDomain() const { return 0x504C4F59 + mSlot; }
Float64 OzzyHALDevice::GetNominalSampleRate() const { return mCurrentStreamFormat.mSampleRate; }
UInt32 OzzyHALDevice::CopySampleRates(AudioValueRange* out, UInt32 max) const {
    UInt32 n = out ? (mHostRateCount < max ? mHostRateCount : max) : mHostRateCount;
    for (UInt32 i = 0; out && i < n; i++) out[i].mMinimum = out[i].mMaximum = mHostRates[i];
    return n;
}
AudioBufferList* OzzyHALDevice::GetInputStreamConfiguration() const { return const_cast<AudioBufferList*>(&mInputConfig); }
AudioBufferList* OzzyHALDevice::GetOutputStreamConfiguration() const { return const_cast<AudioBufferList*>(&mOutputConfig); }
AudioStreamBasicDescription OzzyHALDevice::GetStreamFormat(bool isOutput) const {
    AudioStreamBasicDescription format = mCurrentStreamFormat;
    format.mChannelsPerFrame = isOutput ? mOutputChannels : mInputChannels;
    format.mBytesPerFrame = format.mChannelsPerFrame * (format.mBitsPerChannel / 8);
    format.mBytesPerPacket = format.mBytesPerFrame;
    return format;
}
/* One format per host rate */
UInt32 OzzyHALDevice::CopyStreamRangedDescriptions(bool isOutput, AudioStreamRangedDescription* out, UInt32 max) const {
    UInt32 n = out ? (mHostRateCount < max ? mHostRateCount : max) : mHostRateCount;
    for (UInt32 i = 0; out && i < n; i++) {
        out[i].mFormat = GetStreamFormat(isOutput);
        out[i].mFormat.mSampleRate = mHostRates[i];
        out[i].mSampleRateRange.mMinimum = out[i].mSampleRateRange.mMaximum = mHostRates[i];
    }
    return n;
}

AudioChannelLayout* OzzyHALDevice::GetPreferredChannelLayout(AudioObjectPropertyScope inScope) {
    AudioChannelLayout* layout = (AudioChannelLayout*)mChannelLayoutBuffer;
    layout->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
    layout->mChannelBitmap = 0;
    UInt32 channels = IsOutputScope(inScope) ? mOutputChannels : mInputChannels;
    layout->mNumberChannelDescriptions = channels;
    for (UInt32 i = 0; i < channels; ++i) {
        layout->mChannelDescriptions[i].mChannelLabel = kAudioChannelLabel_Unknown;
        layout->mChannelDescriptions[i].mChannelFlags = kAudioChannelFlags_AllOff;
    }
    return layout;
}

UInt32 OzzyHALDevice::GetPreferredChannelLayoutSize(AudioObjectPropertyScope inScope) const {
    UInt32 channels = IsOutputScope(inScope) ? mOutputChannels : mInputChannels;
    return offsetof(AudioChannelLayout, mChannelDescriptions) + (channels * sizeof(AudioChannelDescription));
}

// ============================================================================
// OzzyHAL (the plug-in): finds OzzyKext instances and gives each a device slot
// ============================================================================

OzzyHAL& OzzyHAL::Get() {
    static OzzyHAL instance;
    return instance;
}

void OzzyHAL::Init(AudioServerPlugInHostRef host) {
    LogOzzyHAL("init (build %{public}s)", OZZY_GIT_VERSION);
    mHost = host;
    for (uint32_t i = 0; i < kOzzyMaxDevices; i++) mDevices[i].Init(i);

    // Pick up devices that are already connected, then watch for changes
    ScanServices();
    mMonitorRunning = true;
    pthread_create(&mMonitorThread, NULL, MonitorEntry, this);
}

void OzzyHAL::Cleanup() {
    mMonitorRunning = false;
    pthread_join(mMonitorThread, NULL);
    for (uint32_t i = 0; i < kOzzyMaxDevices; i++) mDevices[i].Unmap();
}

OzzyHALDevice* OzzyHAL::DeviceForObject(AudioObjectID objectID) {
    if (objectID < kOzzyObjectIDBase) return nullptr;
    uint32_t slot = (objectID - kOzzyObjectIDBase) / kOzzyObjectsPerDevice;
    if (slot >= kOzzyMaxDevices || !mDevices[slot].IsUsed()) return nullptr;
    return &mDevices[slot];
}

UInt32 OzzyHAL::GetDeviceList(AudioObjectID* outIDs, UInt32 maxIDs) const {
    UInt32 count = 0;
    for (uint32_t i = 0; i < kOzzyMaxDevices; i++) {
        if (!mDevices[i].IsConnected()) continue;
        if (outIDs && count < maxIDs) outIDs[count] = mDevices[i].GetDeviceID();
        count++;
    }
    return count;
}

/* A slot for a device with this UID: the one it had before, else a never-used one, else
 * any slot whose device is gone */
OzzyHALDevice* OzzyHAL::SlotForUID(const char* uid) {
    for (uint32_t i = 0; i < kOzzyMaxDevices; i++)
        if (!mDevices[i].IsMapped() && !mDevices[i].IsConnected() && mDevices[i].HasDeviceUID(uid)) return &mDevices[i];
    for (uint32_t i = 0; i < kOzzyMaxDevices; i++)
        if (!mDevices[i].IsUsed()) return &mDevices[i];
    for (uint32_t i = 0; i < kOzzyMaxDevices; i++)
        if (!mDevices[i].IsMapped() && !mDevices[i].IsConnected()) return &mDevices[i];
    return nullptr;
}

void OzzyHAL::ConnectService(io_service_t service, uint64_t registryID) {
    io_connect_t connect = IO_OBJECT_NULL;
    mach_vm_address_t address = 0;
    mach_vm_size_t size = 0;

    if (IOServiceOpen(service, mach_task_self(), 0, &connect) != KERN_SUCCESS) return;
    kern_return_t kr = IOConnectMapMemory(connect, 0, mach_task_self(), &address, &size, kIOMapAnywhere);
    if (kr != KERN_SUCCESS) {
        LogOzzyHALError("failed to map kext memory (0x%{public}x)", kr);
        IOServiceClose(connect);
        return;
    }

    char uid[128];
    uint32_t locationID = OzzyUSBLocationID(service);
    OzzyHALDevice::MakeDeviceUID((const ozzy_shared_memory*)address, locationID, uid, sizeof(uid));
    OzzyHALDevice* device = SlotForUID(uid);
    if (!device) LogOzzyHALError("too many devices, ignoring '%{public}s'", uid);
    if (!device || !device->Map(connect, address, size, registryID, locationID)) {
        IOConnectUnmapMemory(connect, 0, mach_task_self(), address);
        IOServiceClose(connect);
    }
}

/* Attach to OzzyKext instances with audio that no device has yet (skipping MIDI-only
 * devices such as the Xone:1D) */
void OzzyHAL::ScanServices() {
    CFMutableDictionaryRef matching = IOServiceMatching("OzzyKext");
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(kCFAllocatorDefault, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(props, CFSTR(OZZY_AUDIO_PROPERTY_KEY), kCFBooleanTrue);
    CFDictionarySetValue(matching, CFSTR(kIOPropertyMatchKey), props);
    CFRelease(props);

    io_iterator_t it = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(MACH_PORT_NULL, matching, &it) != KERN_SUCCESS) return;
    io_service_t service;
    while ((service = IOIteratorNext(it))) {
        uint64_t registryID = 0;
        IORegistryEntryGetRegistryEntryID(service, &registryID);
        bool known = false;
        for (uint32_t i = 0; i < kOzzyMaxDevices; i++)
            known |= (mDevices[i].IsMapped() && mDevices[i].GetRegistryID() == registryID);
        if (!known) ConnectService(service, registryID);
        IOObjectRelease(service);
    }
    IOObjectRelease(it);
}

void* OzzyHAL::MonitorEntry(void* arg) {
    ((OzzyHAL*)arg)->MonitorLoop();
    return NULL;
}

void OzzyHAL::MonitorLoop() {
    while (mMonitorRunning) {
        // Withdraw departed devices before scanning, so a replugged device finds its old slot free
        bool listChanged = false;
        for (uint32_t i = 0; i < kOzzyMaxDevices; i++) {
            mDevices[i].CheckHealth();
            listChanged |= mDevices[i].UpdateConnectionState(mHost);
        }

        ScanServices();

        bool anyMapped = false;
        for (uint32_t i = 0; i < kOzzyMaxDevices; i++) {
            listChanged |= mDevices[i].UpdateConnectionState(mHost);
            anyMapped |= mDevices[i].IsMapped();
        }
        // The stats: the clients hear of an incident within a round of this loop
        for (uint32_t i = 0; i < kOzzyMaxDevices && mHost; i++) {
            if (!mDevices[i].IsConnected() || !mDevices[i].StatsChanged()) continue;
            AudioObjectPropertyAddress statsAddr = { OZZY_PROPERTY_STATS, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
            mHost->PropertiesChanged(mHost, mDevices[i].GetDeviceID(), 1, &statsAddr);
        }
        if (listChanged && mHost) {
            AudioObjectPropertyAddress listAddr[2] = {
                { kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
                { kAudioObjectPropertyOwnedObjects, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain } };
            mHost->PropertiesChanged(mHost, kAudioObjectPlugInObject, 2, listAddr);
        }

        if (!anyMapped && !mLoggedWaitingForDriver) LogOzzyHAL("waiting for driver...");
        mLoggedWaitingForDriver = !anyMapped;
        usleep(anyMapped ? 250000 : 100000);
    }
}

// ============================================================================
// AudioServerPlugIn entry points
// ============================================================================

enum OzzyObjectKind { kOzzyObjectDevice = 0, kOzzyObjectInputStream = 1, kOzzyObjectOutputStream = 2,
                     kOzzyObjectClockControl = 3 };

static OzzyObjectKind ObjectKind(AudioObjectID objectID) {
    return (OzzyObjectKind)((objectID - kOzzyObjectIDBase) % kOzzyObjectsPerDevice);
}

extern "C" HRESULT OzzyInitialize(AudioServerPlugInDriverRef, AudioServerPlugInHostRef inHost) {
    OzzyHAL::Get().Init(inHost);
    return kAudioHardwareNoError;
}

extern "C" HRESULT OzzyCreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo*, AudioObjectID*) { return kAudioHardwareNoError; }
extern "C" HRESULT OzzyDestroyDevice(AudioServerPlugInDriverRef, AudioObjectID) { return kAudioHardwareNoError; }
extern "C" HRESULT OzzyAddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo) {
    OzzyHALDevice* dev = OzzyHAL::Get().DeviceForObject(inDeviceObjectID);
    if (dev) dev->AddClient(inClientInfo);
    return kAudioHardwareNoError;
}
extern "C" HRESULT OzzyRemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo) {
    OzzyHALDevice* dev = OzzyHAL::Get().DeviceForObject(inDeviceObjectID);
    if (dev && inClientInfo) dev->RemoveClient(inClientInfo->mClientID);
    return kAudioHardwareNoError;
}
extern "C" HRESULT OzzyPerformDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void*) {
    OzzyHALDevice* dev = OzzyHAL::Get().DeviceForObject(inDeviceObjectID);
    return dev ? dev->PerformConfigurationChange(OzzyHAL::Get().GetHost(), inChangeAction) : kAudioHardwareBadObjectError;
}
extern "C" HRESULT OzzyAbortDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*) { return kAudioHardwareNoError; }

extern "C" Boolean OzzyHasProperty(AudioServerPlugInDriverRef, AudioObjectID inObjectID, pid_t, const AudioObjectPropertyAddress* inAddress) {
    if (inObjectID == kAudioObjectPlugInObject) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass:
            case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer:
            case kAudioPlugInPropertyDeviceList: case kAudioObjectPropertyOwnedObjects:
            case kAudioObjectPropertyControlList: return true;
        }
        return false;
    }
    OzzyHALDevice* dev = OzzyHAL::Get().DeviceForObject(inObjectID);
    if (!dev) return false;
    if (ObjectKind(inObjectID) == kOzzyObjectClockControl) {
        if (!dev->HasSetting(OZZY_SETTING_CLOCK_SOURCE)) return false;
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass:
            case kAudioObjectPropertyName: case kAudioObjectPropertyOwner:
            case kAudioObjectPropertyOwnedObjects: case kAudioControlPropertyScope:
            case kAudioControlPropertyElement: case kAudioSelectorControlPropertyCurrentItem:
            case kAudioSelectorControlPropertyAvailableItems: case kAudioSelectorControlPropertyItemName:
            case kAudioSelectorControlPropertyItemKind: return true;
        }
        return false;
    }
    if (ObjectKind(inObjectID) == kOzzyObjectDevice) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass:
            case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer:
            case kAudioObjectPropertyOwner: case kAudioObjectPropertyOwnedObjects:
            case kAudioObjectPropertyControlList:
            case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID:
            case kAudioDevicePropertyTransportType: case kAudioDevicePropertyIsHidden:
            case kAudioDevicePropertyDeviceIsAlive: case kAudioDevicePropertyDeviceIsRunning:
            case kAudioDevicePropertyDeviceIsRunningSomewhere: case kAudioDevicePropertyNominalSampleRate:
            case kAudioDevicePropertyAvailableNominalSampleRates: case kAudioDevicePropertyStreams:
            case kAudioDevicePropertyStreamConfiguration: case kAudioDevicePropertyLatency:
            case kAudioDevicePropertySafetyOffset: case kAudioDevicePropertyPreferredChannelLayout:
            case kAudioDevicePropertyZeroTimeStampPeriod: case kAudioDevicePropertyClockDomain: return true;
            case OZZY_PROPERTY_SETTING: return dev->HasSettings();
            case OZZY_PROPERTY_INFO: case OZZY_PROPERTY_STATS: case OZZY_PROPERTY_CLIENTS:
            case kAudioObjectPropertyCustomPropertyInfoList: return true;
        }
        return false;
    }
    switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass:
        case kAudioObjectPropertyName: case kAudioObjectPropertyOwner:
        case kAudioStreamPropertyDirection: case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel: case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: case kAudioStreamPropertyLatency: return true;
    }
    return false;
}

extern "C" HRESULT OzzyIsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID inObjectID, pid_t, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable) {
    OzzyHALDevice* dev = OzzyHAL::Get().DeviceForObject(inObjectID);
    *outIsSettable = dev && ((ObjectKind(inObjectID) == kOzzyObjectClockControl &&
                              dev->HasSetting(OZZY_SETTING_CLOCK_SOURCE) &&
                              inAddress->mSelector == kAudioSelectorControlPropertyCurrentItem) ||
                             (ObjectKind(inObjectID) == kOzzyObjectDevice &&
                              inAddress->mSelector == OZZY_PROPERTY_SETTING && dev->HasSettings()) ||
                             (ObjectKind(inObjectID) == kOzzyObjectDevice &&
                              inAddress->mSelector == kAudioDevicePropertyNominalSampleRate) ||
                             ((ObjectKind(inObjectID) == kOzzyObjectInputStream || ObjectKind(inObjectID) == kOzzyObjectOutputStream) &&
                              (inAddress->mSelector == kAudioStreamPropertyVirtualFormat ||
                               inAddress->mSelector == kAudioStreamPropertyPhysicalFormat)));
    return kAudioHardwareNoError;
}

extern "C" HRESULT OzzyGetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID inObjectID, pid_t, const AudioObjectPropertyAddress* inAddress, UInt32, const void*, UInt32* outDataSize) {
    if (!outDataSize) return kAudioHardwareIllegalOperationError;
    *outDataSize = 0;

    if (inObjectID == kAudioObjectPlugInObject) {
        switch (inAddress->mSelector) {
            case kAudioPlugInPropertyDeviceList: case kAudioObjectPropertyOwnedObjects:
                *outDataSize = OzzyHAL::Get().GetDeviceList(nullptr, 0) * sizeof(AudioObjectID); return kAudioHardwareNoError;
            case kAudioObjectPropertyControlList: *outDataSize = 0; return kAudioHardwareNoError;
            case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer: *outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
            case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
            default: return kAudioHardwareUnknownPropertyError;
        }
    }
    OzzyHALDevice* dev = OzzyHAL::Get().DeviceForObject(inObjectID);
    if (!dev) return kAudioHardwareBadObjectError;
    UInt32 controls = dev->HasSetting(OZZY_SETTING_CLOCK_SOURCE) ? 1 : 0;
    if (ObjectKind(inObjectID) == kOzzyObjectClockControl) {
        if (!controls) return kAudioHardwareBadObjectError;
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
            case kAudioObjectPropertyName: case kAudioSelectorControlPropertyItemName: *outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
            case kAudioObjectPropertyOwner: *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
            case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; return kAudioHardwareNoError;
            case kAudioControlPropertyScope: case kAudioControlPropertyElement:
            case kAudioSelectorControlPropertyCurrentItem: case kAudioSelectorControlPropertyItemKind:
                *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
            case kAudioSelectorControlPropertyAvailableItems: *outDataSize = OZZY_NUM_CLOCK_SOURCES * sizeof(UInt32); return kAudioHardwareNoError;
            default: return kAudioHardwareUnknownPropertyError;
        }
    }
    if (ObjectKind(inObjectID) == kOzzyObjectDevice) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer:
            case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID:
                *outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
            case kAudioObjectPropertyControlList: *outDataSize = controls * sizeof(AudioObjectID); return kAudioHardwareNoError;
            case kAudioObjectPropertyOwner: *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
            case kAudioObjectPropertyOwnedObjects: *outDataSize = (2 + controls) * sizeof(AudioObjectID); return kAudioHardwareNoError;
            case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
            case kAudioDevicePropertyTransportType: case kAudioDevicePropertyIsHidden:
            case kAudioDevicePropertyDeviceIsAlive: case kAudioDevicePropertyDeviceIsRunning:
            case kAudioDevicePropertyDeviceIsRunningSomewhere: case kAudioDevicePropertyClockDomain:
                *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
            case kAudioDevicePropertyNominalSampleRate: *outDataSize = sizeof(Float64); return kAudioHardwareNoError;
            case kAudioDevicePropertyAvailableNominalSampleRates: *outDataSize = dev->CopySampleRates(nullptr, 0) * sizeof(AudioValueRange); return kAudioHardwareNoError;
            case kAudioDevicePropertyStreams:
                if (inAddress->mScope == kAudioObjectPropertyScopeInput || inAddress->mScope == kAudioObjectPropertyScopeOutput) *outDataSize = sizeof(AudioObjectID);
                else if (inAddress->mScope == kAudioObjectPropertyScopeGlobal) *outDataSize = 2 * sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyStreamConfiguration:
                *outDataSize = GetAudioBufferListSize((IsOutputScope(inAddress->mScope) ? dev->GetOutputStreamConfiguration() : dev->GetInputStreamConfiguration())->mNumberBuffers);
                return kAudioHardwareNoError;
            case kAudioDevicePropertySafetyOffset: case kAudioDevicePropertyLatency: case kAudioDevicePropertyZeroTimeStampPeriod: *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
            case kAudioDevicePropertyPreferredChannelLayout: *outDataSize = dev->GetPreferredChannelLayoutSize(inAddress->mScope); return kAudioHardwareNoError;
            case OZZY_PROPERTY_SETTING:
                if (!dev->HasSettings()) return kAudioHardwareUnknownPropertyError;
                *outDataSize = sizeof(CFPropertyListRef); return kAudioHardwareNoError;
            case OZZY_PROPERTY_INFO: case OZZY_PROPERTY_STATS: case OZZY_PROPERTY_CLIENTS:
                *outDataSize = sizeof(CFPropertyListRef); return kAudioHardwareNoError;
            case kAudioObjectPropertyCustomPropertyInfoList:
                *outDataSize = dev->CustomPropertyCount() * sizeof(AudioServerPlugInCustomPropertyInfo);
                return kAudioHardwareNoError;
            default: return kAudioHardwareUnknownPropertyError;
        }
    }
    switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
        case kAudioObjectPropertyName: *outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
        case kAudioObjectPropertyOwner: *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
        case kAudioStreamPropertyDirection: case kAudioStreamPropertyTerminalType: case kAudioStreamPropertyStartingChannel: case kAudioStreamPropertyLatency:
            *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
        case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat: *outDataSize = sizeof(AudioStreamBasicDescription); return kAudioHardwareNoError;
        case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats:
            *outDataSize = dev->CopyStreamRangedDescriptions(false, nullptr, 0) * sizeof(AudioStreamRangedDescription); return kAudioHardwareNoError;
        default: return kAudioHardwareUnknownPropertyError;
    }
}

extern "C" HRESULT OzzyGetPropertyData(AudioServerPlugInDriverRef, AudioObjectID inObjectID, pid_t, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inMax, UInt32* outSize, void* outData) {
    if (inObjectID == kAudioObjectPlugInObject) {
        switch (inAddress->mSelector) {
            case kAudioPlugInPropertyDeviceList: case kAudioObjectPropertyOwnedObjects: {
                UInt32 maxIDs = outData ? inMax / sizeof(AudioObjectID) : 0;
                UInt32 count = OzzyHAL::Get().GetDeviceList((AudioObjectID*)outData, maxIDs);
                if (count > maxIDs && outData) count = maxIDs;
                if (outSize) *outSize = count * sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            }
            case kAudioObjectPropertyControlList: if (outSize) *outSize = 0; return kAudioHardwareNoError;
            case kAudioObjectPropertyName: return WriteCFString(inMax, outSize, outData, CFSTR("Ozzy HAL"));
            case kAudioObjectPropertyManufacturer: return WriteCFString(inMax, outSize, outData, CFSTR("Ozzy"));
            case kAudioObjectPropertyBaseClass: return WriteClassID(inMax, outSize, outData, kAudioObjectClassID);
            case kAudioObjectPropertyClass: return WriteClassID(inMax, outSize, outData, kAudioPlugInClassID);
            default: return kAudioHardwareUnknownPropertyError;
        }
    }
    OzzyHALDevice* dev = OzzyHAL::Get().DeviceForObject(inObjectID);
    if (!dev) return kAudioHardwareBadObjectError;
    bool hasClock = dev->HasSetting(OZZY_SETTING_CLOCK_SOURCE);
    if (ObjectKind(inObjectID) == kOzzyObjectClockControl) {
        if (!hasClock) return kAudioHardwareBadObjectError;
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass: return WriteClassID(inMax, outSize, outData, kAudioSelectorControlClassID);
            case kAudioObjectPropertyClass: return WriteClassID(inMax, outSize, outData, kAudioClockSourceControlClassID);
            case kAudioObjectPropertyOwner: return WriteObjectID(inMax, outSize, outData, dev->GetDeviceID());
            case kAudioObjectPropertyOwnedObjects: if (outSize) *outSize = 0; return kAudioHardwareNoError;
            case kAudioObjectPropertyName: return WriteCFString(inMax, outSize, outData, CFSTR("Clock Source"));
            case kAudioControlPropertyScope: return WriteUInt32(inMax, outSize, outData, kAudioObjectPropertyScopeGlobal);
            case kAudioControlPropertyElement: return WriteUInt32(inMax, outSize, outData, kAudioObjectPropertyElementMain);
            case kAudioSelectorControlPropertyCurrentItem: return WriteUInt32(inMax, outSize, outData, dev->GetSetting(OZZY_SETTING_CLOCK_SOURCE));
            case kAudioSelectorControlPropertyAvailableItems: {
                UInt32 n = outData ? inMax / sizeof(UInt32) : OZZY_NUM_CLOCK_SOURCES;
                if (n > OZZY_NUM_CLOCK_SOURCES) n = OZZY_NUM_CLOCK_SOURCES;
                for (UInt32 i = 0; outData && i < n; i++) ((UInt32*)outData)[i] = i;
                if (outSize) *outSize = n * sizeof(UInt32);
                return kAudioHardwareNoError;
            }
            case kAudioSelectorControlPropertyItemName: case kAudioSelectorControlPropertyItemKind: {
                if (inQualifierDataSize < sizeof(UInt32) || !inQualifierData) return kAudioHardwareBadPropertySizeError;
                UInt32 item = *(const UInt32*)inQualifierData;
                if (item >= OZZY_NUM_CLOCK_SOURCES) return kAudioHardwareIllegalOperationError;
                if (inAddress->mSelector == kAudioSelectorControlPropertyItemKind)
                    return WriteUInt32(inMax, outSize, outData, item == OZZY_CLOCK_INTERNAL ? kAudioClockSourceItemKindInternal : 0);
                CFStringRef name = dev->CopyClockSourceName(item);
                if (!name) return kAudioHardwareUnspecifiedError;
                OSStatus st = WriteCFString(inMax, outSize, outData, name);   // retains it for the caller
                CFRelease(name);
                return st;
            }
            default: return kAudioHardwareUnknownPropertyError;
        }
    }
    if (ObjectKind(inObjectID) == kOzzyObjectDevice) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass: return WriteClassID(inMax, outSize, outData, kAudioObjectClassID);
            case kAudioObjectPropertyClass: return WriteClassID(inMax, outSize, outData, kAudioDeviceClassID);
            case kAudioObjectPropertyControlList:
                if (!hasClock) { if (outSize) *outSize = 0; return kAudioHardwareNoError; }
                return WriteObjectID(inMax, outSize, outData, dev->GetClockControlID());
            case kAudioObjectPropertyOwner: return WriteObjectID(inMax, outSize, outData, kAudioObjectPlugInObject);
            case kAudioObjectPropertyName: return WriteCFString(inMax, outSize, outData, dev->GetDeviceName());
            case kAudioObjectPropertyManufacturer: return WriteCFString(inMax, outSize, outData, dev->GetManufacturer());
            case kAudioDevicePropertyDeviceUID: return WriteCFString(inMax, outSize, outData, dev->GetDeviceUID());
            case kAudioDevicePropertyModelUID: return WriteCFString(inMax, outSize, outData, dev->GetModelUID());
            case kAudioDevicePropertyTransportType: return WriteUInt32(inMax, outSize, outData, dev->GetTransportType());
            case kAudioDevicePropertyIsHidden: return WriteUInt32(inMax, outSize, outData, 0);
            case kAudioDevicePropertyDeviceIsAlive: return WriteUInt32(inMax, outSize, outData, dev->IsConnected() ? 1 : 0);
            case kAudioDevicePropertyDeviceIsRunning: case kAudioDevicePropertyDeviceIsRunningSomewhere: return WriteUInt32(inMax, outSize, outData, dev->IsRunning() ? 1 : 0);
            case kAudioDevicePropertyNominalSampleRate: return WriteFloat64(inMax, outSize, outData, dev->GetNominalSampleRate());
            case kAudioDevicePropertyAvailableNominalSampleRates: {
                UInt32 n = dev->CopySampleRates((AudioValueRange*)outData, inMax / sizeof(AudioValueRange));
                if (outSize) *outSize = n * sizeof(AudioValueRange);
                return kAudioHardwareNoError;
            }
            case kAudioDevicePropertyStreams: {
                UInt32 streamList[] = { dev->GetInputStreamID(), dev->GetOutputStreamID() };
                if (inAddress->mScope == kAudioObjectPropertyScopeInput) return WriteObjectID(inMax, outSize, outData, dev->GetInputStreamID());
                else if (inAddress->mScope == kAudioObjectPropertyScopeOutput) return WriteObjectID(inMax, outSize, outData, dev->GetOutputStreamID());
                else { if (outData && inMax < sizeof(streamList)) return kAudioHardwareBadPropertySizeError; if (outData) memcpy(outData, streamList, sizeof(streamList)); if (outSize) *outSize = sizeof(streamList); return kAudioHardwareNoError; }
            }
            case kAudioObjectPropertyOwnedObjects: {
                UInt32 count = hasClock ? 3 : 2;
                if (outData && inMax < count * sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                AudioObjectID* list = (AudioObjectID*)outData;
                if (list) { list[0] = dev->GetOutputStreamID(); list[1] = dev->GetInputStreamID(); if (hasClock) list[2] = dev->GetClockControlID(); }
                if (outSize) *outSize = count * sizeof(AudioObjectID); return kAudioHardwareNoError;
            }
            case kAudioDevicePropertyStreamConfiguration: {
                AudioBufferList* config = (inAddress->mScope == kAudioObjectPropertyScopeInput) ? dev->GetInputStreamConfiguration() : dev->GetOutputStreamConfiguration();
                UInt32 actualSize = GetAudioBufferListSize(config->mNumberBuffers); if (outSize) *outSize = actualSize;
                if (outData) { if (inMax < actualSize) return kAudioHardwareBadPropertySizeError; memcpy(outData, config, actualSize); } return kAudioHardwareNoError;
            }
            case kAudioDevicePropertyClockDomain: return WriteUInt32(inMax, outSize, outData, dev->GetClockDomain());
            case kAudioDevicePropertyLatency: return WriteUInt32(inMax, outSize, outData, dev->GetLatency());
            case kAudioDevicePropertySafetyOffset: return WriteUInt32(inMax, outSize, outData, dev->GetSafetyOffset(inAddress->mScope));
            case kAudioDevicePropertyZeroTimeStampPeriod: return WriteUInt32(inMax, outSize, outData, dev->GetZeroTimestampPeriod());
            case kAudioDevicePropertyPreferredChannelLayout: {
                UInt32 size = dev->GetPreferredChannelLayoutSize(inAddress->mScope);
                if (outSize) *outSize = size;
                if (outData) { if (inMax < size) return kAudioHardwareBadPropertySizeError; memcpy(outData, dev->GetPreferredChannelLayout(inAddress->mScope), size); }
                return kAudioHardwareNoError;
            }
            case OZZY_PROPERTY_SETTING: {
                if (!dev->HasSettings()) return kAudioHardwareUnknownPropertyError;
                if (outData && inMax < sizeof(CFPropertyListRef)) return kAudioHardwareBadPropertySizeError;
                if (outData) *(CFPropertyListRef*)outData = dev->CopySettingsDictionary();   // the caller releases it
                if (outSize) *outSize = sizeof(CFPropertyListRef);
                return kAudioHardwareNoError;
            }
            case OZZY_PROPERTY_INFO: {
                if (outData && inMax < sizeof(CFPropertyListRef)) return kAudioHardwareBadPropertySizeError;
                if (outData) *(CFPropertyListRef*)outData = dev->CopyInfoData();   // the caller releases it
                if (outSize) *outSize = sizeof(CFPropertyListRef);
                return kAudioHardwareNoError;
            }
            case OZZY_PROPERTY_STATS: {
                if (outData && inMax < sizeof(CFPropertyListRef)) return kAudioHardwareBadPropertySizeError;
                if (outData) *(CFPropertyListRef*)outData = dev->CopyStatsData();   // the caller releases it
                if (outSize) *outSize = sizeof(CFPropertyListRef);
                return kAudioHardwareNoError;
            }
            case OZZY_PROPERTY_CLIENTS: {
                if (outData && inMax < sizeof(CFPropertyListRef)) return kAudioHardwareBadPropertySizeError;
                if (outData) *(CFPropertyListRef*)outData = dev->CopyClientsArray();   // the caller releases it
                if (outSize) *outSize = sizeof(CFPropertyListRef);
                return kAudioHardwareNoError;
            }
            case kAudioObjectPropertyCustomPropertyInfoList: {
                // OZZY_PROPERTY_INFO, OZZY_PROPERTY_STATS, OZZY_PROPERTY_CLIENTS, and
                // OZZY_PROPERTY_SETTING with settings: property list data, no qualifier
                UInt32 count = dev->CustomPropertyCount();
                UInt32 size = count * sizeof(AudioServerPlugInCustomPropertyInfo);
                if (outData && inMax < size) return kAudioHardwareBadPropertySizeError;
                if (outData) {
                    AudioServerPlugInCustomPropertyInfo* info = (AudioServerPlugInCustomPropertyInfo*)outData;
                    for (UInt32 i = 0; i < count; i++) {
                        info[i].mSelector = i == 0 ? OZZY_PROPERTY_INFO : i == 1 ? OZZY_PROPERTY_STATS :
                                            i == 2 ? OZZY_PROPERTY_CLIENTS : OZZY_PROPERTY_SETTING;
                        info[i].mPropertyDataType = kAudioServerPlugInCustomPropertyDataTypeCFPropertyList;
                        info[i].mQualifierDataType = kAudioServerPlugInCustomPropertyDataTypeNone;
                    }
                }
                if (outSize) *outSize = size;
                return kAudioHardwareNoError;
            }
            default: return kAudioHardwareUnknownPropertyError;
        }
    }
    bool isOutput = (ObjectKind(inObjectID) == kOzzyObjectOutputStream);
    switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass: return WriteClassID(inMax, outSize, outData, kAudioObjectClassID);
        case kAudioObjectPropertyClass: return WriteClassID(inMax, outSize, outData, kAudioStreamClassID);
        case kAudioObjectPropertyName: return WriteCFString(inMax, outSize, outData, (isOutput ? CFSTR("Ozzy PCM Output Stream") : CFSTR("Ozzy PCM Input Stream")));
        case kAudioObjectPropertyOwner: return WriteObjectID(inMax, outSize, outData, dev->GetDeviceID());
        case kAudioStreamPropertyDirection: return WriteUInt32(inMax, outSize, outData, (isOutput ? 0 : 1));
        case kAudioStreamPropertyTerminalType: return WriteUInt32(inMax, outSize, outData, kAudioStreamTerminalTypeSpeaker);
        case kAudioStreamPropertyStartingChannel: return WriteUInt32(inMax, outSize, outData, 1);
        case kAudioStreamPropertyLatency: return WriteUInt32(inMax, outSize, outData, dev->GetLatency());
        case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat:
            if (outData && inMax < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
            if (outData) *(AudioStreamBasicDescription*)outData = dev->GetStreamFormat(isOutput);
            if (outSize) *outSize = sizeof(AudioStreamBasicDescription); return kAudioHardwareNoError;
        case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats: {
            UInt32 n = dev->CopyStreamRangedDescriptions(isOutput, (AudioStreamRangedDescription*)outData,
                                                         inMax / sizeof(AudioStreamRangedDescription));
            if (outSize) *outSize = n * sizeof(AudioStreamRangedDescription);
            return kAudioHardwareNoError;
        }
        default: return kAudioHardwareUnknownPropertyError;
    }
}

extern "C" HRESULT OzzySetPropertyData(AudioServerPlugInDriverRef, AudioObjectID inObjectID, pid_t, const AudioObjectPropertyAddress* inAddress, UInt32, const void*, UInt32 inDataSize, const void* inData) {
    OzzyHALDevice* dev = OzzyHAL::Get().DeviceForObject(inObjectID);
    if (!dev) return kAudioHardwareBadObjectError;
    if (ObjectKind(inObjectID) == kOzzyObjectClockControl && dev->HasSetting(OZZY_SETTING_CLOCK_SOURCE) &&
        inAddress->mSelector == kAudioSelectorControlPropertyCurrentItem) {
        if (inDataSize < sizeof(UInt32) || !inData) return kAudioHardwareBadPropertySizeError;
        return dev->SetSetting(OzzyHAL::Get().GetHost(), OZZY_SETTING_CLOCK_SOURCE, *(const UInt32*)inData);
    }
    if (ObjectKind(inObjectID) == kOzzyObjectDevice && inAddress->mSelector == OZZY_PROPERTY_SETTING &&
        dev->HasSettings()) {
        if (inDataSize < sizeof(CFPropertyListRef) || !inData) return kAudioHardwareBadPropertySizeError;
        return dev->SetSettingsDictionary(OzzyHAL::Get().GetHost(), *(const CFDictionaryRef*)inData);
    }
    if (ObjectKind(inObjectID) == kOzzyObjectDevice && inAddress->mSelector == kAudioDevicePropertyNominalSampleRate) {
        if (inDataSize < sizeof(Float64) || !inData) return kAudioHardwareBadPropertySizeError;
        return dev->SetSampleRate(OzzyHAL::Get().GetHost(), *(const Float64*)inData);
    }
    if ((ObjectKind(inObjectID) == kOzzyObjectInputStream || ObjectKind(inObjectID) == kOzzyObjectOutputStream) &&
        (inAddress->mSelector == kAudioStreamPropertyVirtualFormat || inAddress->mSelector == kAudioStreamPropertyPhysicalFormat)) {
        if (inDataSize < sizeof(AudioStreamBasicDescription) || !inData) return kAudioHardwareBadPropertySizeError;
        return dev->SetSampleRate(OzzyHAL::Get().GetHost(), ((const AudioStreamBasicDescription*)inData)->mSampleRate);
    }
    return kAudioHardwareUnsupportedOperationError;
}
extern "C" HRESULT OzzyStartIO(AudioServerPlugInDriverRef, AudioObjectID inDeviceObjectID, UInt32 inClientID) {
    OzzyHALDevice* dev = OzzyHAL::Get().DeviceForObject(inDeviceObjectID);
    return dev ? dev->StartIO(OzzyHAL::Get().GetHost(), inClientID) : kAudioHardwareBadObjectError;
}
extern "C" HRESULT OzzyStopIO(AudioServerPlugInDriverRef, AudioObjectID inDeviceObjectID, UInt32 inClientID) {
    OzzyHALDevice* dev = OzzyHAL::Get().DeviceForObject(inDeviceObjectID);
    return dev ? dev->StopIO(OzzyHAL::Get().GetHost(), inClientID) : kAudioHardwareBadObjectError;
}
extern "C" HRESULT OzzyGetZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID inDeviceObjectID, UInt32, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed) {
    OzzyHALDevice* dev = OzzyHAL::Get().DeviceForObject(inDeviceObjectID);
    return dev ? dev->GetZeroTimeStamp(outSampleTime, outHostTime, outSeed) : kAudioHardwareBadObjectError;
}
extern "C" OSStatus OzzyWillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32 inOperationID, Boolean* outWillDo, Boolean* outWillDoInPlace) { bool willDo = false; bool willDoInPlace = true; switch (inOperationID) { case kAudioServerPlugInIOOperationWriteMix: case kAudioServerPlugInIOOperationReadInput: willDo = true; break; default: willDo = false; break; } if (outWillDo) *outWillDo = willDo; if (outWillDoInPlace) *outWillDoInPlace = willDoInPlace; return kAudioHardwareNoError; }
extern "C" OSStatus OzzyBeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*) { return 0; }
extern "C" OSStatus OzzyDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID inDeviceObjectID, UInt32, UInt32, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void*) {
    OzzyHALDevice* dev = OzzyHAL::Get().DeviceForObject(inDeviceObjectID);
    return dev ? dev->ioOperation(inOperationID, inIOBufferFrameSize, inIOCycleInfo, ioMainBuffer) : kAudioHardwareBadObjectError;
}
extern "C" OSStatus OzzyEndIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*) { return 0; }
extern "C" HRESULT OzzyQueryInterface(void*, REFIID inREFIID, LPVOID* outInterface) { extern AudioServerPlugInDriverRef gOzzyDriverRef; CFUUIDBytes bytes = CFUUIDGetUUIDBytes(kAudioServerPlugInDriverInterfaceUUID); if (outInterface && memcmp(&inREFIID, &bytes, sizeof(REFIID)) == 0) { *outInterface = gOzzyDriverRef; return kAudioHardwareNoError; } return kAudioHardwareUnknownPropertyError; }
extern "C" ULONG OzzyAddRef(void*) { return 1; }
extern "C" ULONG OzzyRelease(void*) { return 1; }

static AudioServerPlugInDriverInterface gOzzyInterface = { NULL, OzzyQueryInterface, OzzyAddRef, OzzyRelease, OzzyInitialize, OzzyCreateDevice, OzzyDestroyDevice, OzzyAddDeviceClient, OzzyRemoveDeviceClient, OzzyPerformDeviceConfigurationChange, OzzyAbortDeviceConfigurationChange, OzzyHasProperty, OzzyIsPropertySettable, OzzyGetPropertyDataSize, OzzyGetPropertyData, OzzySetPropertyData, OzzyStartIO, OzzyStopIO, OzzyGetZeroTimeStamp, OzzyWillDoIOOperation, OzzyBeginIOOperation, OzzyDoIOOperation, OzzyEndIOOperation };
static AudioServerPlugInDriverInterface* gOzzyInterfacePtr = &gOzzyInterface;
AudioServerPlugInDriverRef gOzzyDriverRef = &gOzzyInterfacePtr;

extern "C" __attribute__((visibility("default"))) void* OzzyPluginFactory(CFAllocatorRef, CFUUIDRef inRequestedTypeUUID) {
	if (CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID)) return (void*)gOzzyDriverRef;
	return NULL;
}