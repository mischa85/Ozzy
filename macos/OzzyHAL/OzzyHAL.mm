#include "OzzyHAL.h"
#include "../Devices/Ploytec/PloytecCodec.h"
#include "../Devices/Ploytec/PloytecProtocol.h"
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

    mAvailableSampleRates.mMinimum = 96000.0;
    mAvailableSampleRates.mMaximum = 96000.0;

    RebuildStreamConfigs();
}

void OzzyHALDevice::ClearOutputBuffer() {
    if (!mOutputBufferAddr || !mSHM || !mClearOutput) return;
    const uint32_t bufferSize = kOzzyMaxPacketSize * kOzzyNumPackets;
    mClearOutput(mOutputBufferAddr, bufferSize);
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
void OzzyHALDevice::MakeDeviceUID(const OzzySharedMemory* shm, uint32_t locationID, char* buf, size_t size) {
    bool hasSerial = shm->serialNumber[0] != 0 && strcmp(shm->serialNumber, "no serial number") != 0;
    if (hasSerial) snprintf(buf, size, "OzzyHAL.%s", shm->serialNumber);
    else snprintf(buf, size, "OzzyHAL.%04X:%04X.%08X", shm->vendorID, shm->productID, locationID);
}

bool OzzyHALDevice::HasDeviceUID(const char* uid) const {
    if (!mDeviceUID) return false;
    char buf[128];
    return CFStringGetCString(mDeviceUID, buf, sizeof(buf), kCFStringEncodingUTF8) && strcmp(buf, uid) == 0;
}

/* Take over a mapped kext connection. Returns false (and leaves it to the caller to close)
 * if the shared memory isn't usable. */
bool OzzyHALDevice::Map(io_connect_t connect, mach_vm_address_t address, mach_vm_size_t size, uint64_t registryID, uint32_t locationID) {
    OzzySharedMemory* shm = (OzzySharedMemory*)address;
    if (shm->magic != kOzzyMagic) {
        LogOzzyHALError("SHM magic mismatch");
        return false;
    }
    if (shm->version != kOzzySharedVersion) {
        LogOzzyHALError("SHM version %{public}u, expected %{public}u: reinstall the kext and HAL together",
                        shm->version, kOzzySharedVersion);
        return false;
    }
    if (shm->vendorID == 0) return false;

    mKextConnect = connect;
    mKextMapAddress = address;
    mKextMapSize = size;
    mRegistryID = registryID;
    mLocationID = locationID;
    mSessionID = shm->sessionID;
    mInputBufferAddr = shm->audio.inputBuffer;
    mOutputBufferAddr = shm->audio.outputBuffer;
    mSHM = shm;
    LogOzzyHAL("device %{public}u: attached to kext (session: 0x%{public}08X)", mSlot, mSessionID);
    return true;
}

void OzzyHALDevice::Unmap() {
    if (!mSHM) return;
    mSHM = nullptr;
    mInputBufferAddr = nullptr;
    mOutputBufferAddr = nullptr;
    IOConnectUnmapMemory(mKextConnect, 0, mach_task_self(), mKextMapAddress);
    IOServiceClose(mKextConnect);
    mKextConnect = IO_OBJECT_NULL;
    mKextMapAddress = 0;
    mKextMapSize = 0;
    mRegistryID = 0;
}

/* Unmap if the kext went away or restarted. Returns true while still mapped. */
bool OzzyHALDevice::CheckHealth() {
    if (!mSHM) return false;
    bool magicValid = (mSHM->magic == kOzzyMagic) && (mSHM->sessionID == mSessionID);
    bool hwPresent = magicValid && mSHM->audio.hardwarePresent.load(std::memory_order_acquire);
    if (magicValid && hwPresent) return true;
    LogOzzyHAL("device %{public}u: disconnected (magic: %{public}d, hw: %{public}d)", mSlot, magicValid, hwPresent);
    Unmap();
    return false;
}

/* Select codec and sample rate from what the engine published in shared memory.
 * Called when the device comes up, because the engine only writes deviceFlags and
 * sampleRate during its Start() handshake. */
void OzzyHALDevice::ApplyDeviceConfig() {
    if (!mSHM) return;

    // Cache frame format for real-time I/O performance
    mFramesPerPacket = mSHM->audio.framesPerPacket;  // e.g., 80 frames per logical packet
    // Ring buffer size in FRAMES: 128 logical packets * 80 frames = 10,240 frames
    mRingSize = kOzzyNumPackets * mFramesPerPacket;
    mOutputBytesPerFrame = mSHM->audio.outputBytesPerFrame;
    mInputBytesPerFrame = mSHM->audio.inputBytesPerFrame;

    // Channel counts per direction, as published by the engine for this model
    mInputChannels = mSHM->audio.inputChannels;
    mOutputChannels = mSHM->audio.outputChannels;

    // Bit 0: 512-byte "bulk" output layout (follows wMaxPacketSize, not transfer type).
    // Writer and reader are picked here once; the I/O path doesn't branch on layout or channels.
    bool isBulk = (mSHM->audio.deviceFlags & 1);
    mWriteOutput = PloytecOutputWriter(isBulk, mOutputChannels);
    mReadInput = PloytecInputReader(mInputChannels);
    if (!mWriteOutput || !mReadInput) {
        LogOzzyHALError("no codec for %{public}u in / %{public}u out, falling back to 8 / 8",
                        mInputChannels, mOutputChannels);
        mInputChannels = mOutputChannels = 8;
        mWriteOutput = PloytecOutputWriter(isBulk, 8);
        mReadInput = PloytecInputReader(8);
    }
    mClearOutput = isBulk ? PloytecClearOutputBulk : PloytecClearOutputInterrupt;
    RebuildStreamConfigs();
    LogOzzyHAL("device %{public}u: Ploytec codec (%{public}s layout, %{public}u in / %{public}u out) for VID:0x%{public}04X",
               mSlot, isBulk ? "bulk" : "interrupt", mInputChannels, mOutputChannels, mSHM->vendorID);

    uint32_t rate = mSHM->audio.sampleRate.load();
    if (rate == 0) rate = 96000;
    mCurrentStreamFormat.mSampleRate = rate;
    mAvailableSampleRates.mMinimum = rate;
    mAvailableSampleRates.mMaximum = rate;
    LogOzzyHAL("device %{public}u: sample rate %{public}u Hz", mSlot, rate);
}

void OzzyHALDevice::UpdateMetadata() {
    if (mSHM->productName[0] != 0) {
        if (mModelUID) CFRelease(mModelUID);
        mModelUID = SafeCreateString(mSHM->productName);
    }
    if (mSHM->manufacturerName[0] != 0) {
        if (mManufacturerUID) CFRelease(mManufacturerUID);
        mManufacturerUID = SafeCreateString(mSHM->manufacturerName);
    }
    char uid[128];
    MakeDeviceUID(mSHM, mLocationID, uid, sizeof(uid));
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
        AudioObjectPropertyAddress fmtAddr[2] = {
            { kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
            { kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain } };
        host->PropertiesChanged(host, GetInputStreamID(), 2, fmtAddr);
        host->PropertiesChanged(host, GetOutputStreamID(), 2, fmtAddr);
    } else {
        // Device disconnected - notify that it is no longer alive; the caller updates the list
        AudioObjectPropertyAddress aliveAddr = { kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        host->PropertiesChanged(host, dev, 1, &aliveAddr);
    }
}

/* Publish the device once the engine is up, withdraw it when it goes down */
bool OzzyHALDevice::UpdateConnectionState(AudioServerPlugInHostRef host) {
    bool hwReady = (mSHM && mSHM->audio.hardwarePresent.load(std::memory_order_acquire));
    bool drvReady = (mSHM && mSHM->audio.driverReady.load(std::memory_order_acquire));
    bool metaReady = (mSHM && mSHM->productName[0] != 0);
    bool currentlyConnected = (hwReady && drvReady && metaReady);
    if (currentlyConnected == mConnected) return false;

    if (currentlyConnected) {
        UpdateMetadata();
        ApplyDeviceConfig();
        LogOzzyHAL("device %{public}u ready: '%{public}s'", mSlot, mSHM->productName);
    } else if (mIOStarted) {
        mIOStarted = false;
        mTimestampSeed++;
        LogOzzyHAL("device %{public}u: auto-stopped I/O due to disconnect", mSlot);
    }

    mConnected = currentlyConnected;
    LogOzzyHAL("device %{public}u: connection %{public}s", mSlot, currentlyConnected ? "UP" : "DOWN");
    if (host) NotifyDeviceChanged(host);
    return true;
}

kern_return_t OzzyHALDevice::StartIO() {
    if (!mConnected) return kAudioHardwareNotRunningError;
    if (mSHM) {
        mSHM->audio.halWritePosition.store(0, std::memory_order_relaxed);
    }
    LogOzzyHAL("device %{public}u: start I/O", mSlot);
    mIOStarted = true;
    mTimestampSeed++;
    return kIOReturnSuccess;
}

kern_return_t OzzyHALDevice::StopIO() {
    LogOzzyHAL("device %{public}u: stop I/O", mSlot);
    mIOStarted = false;
    mTimestampSeed++;
    ClearOutputBuffer();
    return kIOReturnSuccess;
}

HRESULT OzzyHALDevice::GetZeroTimeStamp(Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed) {
    if (!mSHM) return kAudioHardwareNotRunningError;

    auto& ts = mSHM->audio.timestamp;
    uint64_t sTime = 0, hTime = 0;
    uint32_t seq1, seq2;
    int retries = 0;

    do {
        seq1 = ts.sequence.load(std::memory_order_acquire);
        if (seq1 & 1) {
            seq2 = seq1 + 1;    // writer busy: retry
            continue;
        }
        sTime = ts.sampleTime.load(std::memory_order_relaxed);
        hTime = ts.hostTime.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire); 
        seq2 = ts.sequence.load(std::memory_order_relaxed);
    } while (seq1 != seq2 && ++retries < 100);

    if (outSampleTime) *outSampleTime = (Float64)sTime;
    if (outHostTime) *outHostTime = hTime;
    if (outSeed) *outSeed = mTimestampSeed;
    
    return kAudioHardwareNoError;
}

OSStatus OzzyHALDevice::ioOperation(UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer) {
    if (!mSHM || !mInputBufferAddr || !mOutputBufferAddr) return kAudioHardwareNoError;

    if (inOperationID == kAudioServerPlugInIOOperationWriteMix) {
        if (mWriteOutput) {
            uint64_t sampleTime = (uint64_t)inIOCycleInfo->mOutputTime.mSampleTime;
            mWriteOutput(mOutputBufferAddr, (const float*)ioMainBuffer, sampleTime, inIOBufferFrameSize, mRingSize, mOutputBytesPerFrame);
            mSHM->audio.halWritePosition.store(sampleTime + inIOBufferFrameSize, std::memory_order_release);
        }
    }
    else if (inOperationID == kAudioServerPlugInIOOperationReadInput) {
        if (mReadInput) {
            uint64_t sampleTime = (uint64_t)inIOCycleInfo->mInputTime.mSampleTime;
            mReadInput((float*)ioMainBuffer, mInputBufferAddr, sampleTime, inIOBufferFrameSize, mRingSize, mInputBytesPerFrame);
        }
    }
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
AudioValueRange OzzyHALDevice::GetAvailableSampleRates() const { return mAvailableSampleRates; }
AudioBufferList* OzzyHALDevice::GetInputStreamConfiguration() const { return const_cast<AudioBufferList*>(&mInputConfig); }
AudioBufferList* OzzyHALDevice::GetOutputStreamConfiguration() const { return const_cast<AudioBufferList*>(&mOutputConfig); }
AudioStreamBasicDescription OzzyHALDevice::GetStreamFormat(bool isOutput) const {
    AudioStreamBasicDescription format = mCurrentStreamFormat;
    format.mChannelsPerFrame = isOutput ? mOutputChannels : mInputChannels;
    format.mBytesPerFrame = format.mChannelsPerFrame * (format.mBitsPerChannel / 8);
    format.mBytesPerPacket = format.mBytesPerFrame;
    return format;
}
AudioStreamRangedDescription OzzyHALDevice::GetStreamRangedDescription(bool isOutput) const {
    AudioStreamRangedDescription outDesc; outDesc.mFormat = GetStreamFormat(isOutput);
    outDesc.mSampleRateRange = mAvailableSampleRates;
    return outDesc;
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
    OzzyHALDevice::MakeDeviceUID((const OzzySharedMemory*)address, locationID, uid, sizeof(uid));
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
    CFDictionarySetValue(props, CFSTR(kOzzyAudioPropertyKey), kCFBooleanTrue);
    CFDictionarySetValue(matching, CFSTR(kIOPropertyMatchKey), props);
    CFRelease(props);

    io_iterator_t it = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &it) != KERN_SUCCESS) return;
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

enum OzzyObjectKind { kOzzyObjectDevice = 0, kOzzyObjectInputStream = 1, kOzzyObjectOutputStream = 2 };

static OzzyObjectKind ObjectKind(AudioObjectID objectID) {
    return (OzzyObjectKind)((objectID - kOzzyObjectIDBase) % kOzzyObjectsPerDevice);
}

extern "C" HRESULT OzzyInitialize(AudioServerPlugInDriverRef, AudioServerPlugInHostRef inHost) {
    OzzyHAL::Get().Init(inHost);
    return kAudioHardwareNoError;
}

extern "C" HRESULT OzzyCreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo*, AudioObjectID*) { return kAudioHardwareNoError; }
extern "C" HRESULT OzzyDestroyDevice(AudioServerPlugInDriverRef, AudioObjectID) { return kAudioHardwareNoError; }
extern "C" HRESULT OzzyAddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*) { return kAudioHardwareNoError; }
extern "C" HRESULT OzzyRemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*) { return kAudioHardwareNoError; }
extern "C" HRESULT OzzyPerformDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*) { return kAudioHardwareNoError; }
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
    if (!OzzyHAL::Get().DeviceForObject(inObjectID)) return false;
    if (ObjectKind(inObjectID) == kOzzyObjectDevice) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass:
            case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer:
            case kAudioObjectPropertyOwner: case kAudioObjectPropertyOwnedObjects:
            case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID:
            case kAudioDevicePropertyTransportType: case kAudioDevicePropertyIsHidden:
            case kAudioDevicePropertyDeviceIsAlive: case kAudioDevicePropertyDeviceIsRunning:
            case kAudioDevicePropertyDeviceIsRunningSomewhere: case kAudioDevicePropertyNominalSampleRate:
            case kAudioDevicePropertyAvailableNominalSampleRates: case kAudioDevicePropertyStreams:
            case kAudioDevicePropertyStreamConfiguration: case kAudioDevicePropertyLatency:
            case kAudioDevicePropertySafetyOffset: case kAudioDevicePropertyPreferredChannelLayout:
            case kAudioDevicePropertyZeroTimeStampPeriod: case kAudioDevicePropertyClockDomain: return true;
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

extern "C" HRESULT OzzyIsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, Boolean* outIsSettable) {
    *outIsSettable = false; return kAudioHardwareNoError;
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
    if (ObjectKind(inObjectID) == kOzzyObjectDevice) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer:
            case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID:
                *outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
            case kAudioObjectPropertyControlList: *outDataSize = 0; return kAudioHardwareNoError;
            case kAudioObjectPropertyOwner: *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
            case kAudioObjectPropertyOwnedObjects: *outDataSize = 2 * sizeof(AudioObjectID); return kAudioHardwareNoError;
            case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
            case kAudioDevicePropertyTransportType: case kAudioDevicePropertyIsHidden:
            case kAudioDevicePropertyDeviceIsAlive: case kAudioDevicePropertyDeviceIsRunning:
            case kAudioDevicePropertyDeviceIsRunningSomewhere: case kAudioDevicePropertyClockDomain:
                *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
            case kAudioDevicePropertyNominalSampleRate: *outDataSize = sizeof(Float64); return kAudioHardwareNoError;
            case kAudioDevicePropertyAvailableNominalSampleRates: *outDataSize = sizeof(AudioValueRange); return kAudioHardwareNoError;
            case kAudioDevicePropertyStreams:
                if (inAddress->mScope == kAudioObjectPropertyScopeInput || inAddress->mScope == kAudioObjectPropertyScopeOutput) *outDataSize = sizeof(AudioObjectID);
                else if (inAddress->mScope == kAudioObjectPropertyScopeGlobal) *outDataSize = 2 * sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyStreamConfiguration:
                *outDataSize = GetAudioBufferListSize((IsOutputScope(inAddress->mScope) ? dev->GetOutputStreamConfiguration() : dev->GetInputStreamConfiguration())->mNumberBuffers);
                return kAudioHardwareNoError;
            case kAudioDevicePropertySafetyOffset: case kAudioDevicePropertyLatency: case kAudioDevicePropertyZeroTimeStampPeriod: *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
            case kAudioDevicePropertyPreferredChannelLayout: *outDataSize = dev->GetPreferredChannelLayoutSize(inAddress->mScope); return kAudioHardwareNoError;
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
        case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats: *outDataSize = sizeof(AudioStreamRangedDescription); return kAudioHardwareNoError;
        default: return kAudioHardwareUnknownPropertyError;
    }
}

extern "C" HRESULT OzzyGetPropertyData(AudioServerPlugInDriverRef, AudioObjectID inObjectID, pid_t, const AudioObjectPropertyAddress* inAddress, UInt32, const void*, UInt32 inMax, UInt32* outSize, void* outData) {
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
    if (ObjectKind(inObjectID) == kOzzyObjectDevice) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass: return WriteClassID(inMax, outSize, outData, kAudioObjectClassID);
            case kAudioObjectPropertyClass: return WriteClassID(inMax, outSize, outData, kAudioDeviceClassID);
            case kAudioObjectPropertyControlList: if (outSize) *outSize = 0; return kAudioHardwareNoError;
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
            case kAudioDevicePropertyAvailableNominalSampleRates: if (outSize) *outSize = sizeof(AudioValueRange); if (outData) { if (inMax < sizeof(AudioValueRange)) return kAudioHardwareBadPropertySizeError; *(AudioValueRange*)outData = dev->GetAvailableSampleRates(); } return kAudioHardwareNoError;
            case kAudioDevicePropertyStreams: {
                UInt32 streamList[] = { dev->GetInputStreamID(), dev->GetOutputStreamID() };
                if (inAddress->mScope == kAudioObjectPropertyScopeInput) return WriteObjectID(inMax, outSize, outData, dev->GetInputStreamID());
                else if (inAddress->mScope == kAudioObjectPropertyScopeOutput) return WriteObjectID(inMax, outSize, outData, dev->GetOutputStreamID());
                else { if (outData && inMax < sizeof(streamList)) return kAudioHardwareBadPropertySizeError; if (outData) memcpy(outData, streamList, sizeof(streamList)); if (outSize) *outSize = sizeof(streamList); return kAudioHardwareNoError; }
            }
            case kAudioObjectPropertyOwnedObjects: {
                if (outData && inMax < 2 * sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                AudioObjectID* list = (AudioObjectID*)outData; if (list) { list[0] = dev->GetOutputStreamID(); list[1] = dev->GetInputStreamID(); }
                if (outSize) *outSize = 2 * sizeof(AudioObjectID); return kAudioHardwareNoError;
            }
            case kAudioDevicePropertyStreamConfiguration: {
                AudioBufferList* config = (inAddress->mScope == kAudioObjectPropertyScopeInput) ? dev->GetInputStreamConfiguration() : dev->GetOutputStreamConfiguration();
                UInt32 actualSize = GetAudioBufferListSize(config->mNumberBuffers); if (outSize) *outSize = actualSize;
                if (outData) { if (inMax < actualSize) return kAudioHardwareBadPropertySizeError; memcpy(outData, config, actualSize); } return kAudioHardwareNoError;
            }
            case kAudioDevicePropertyClockDomain: return WriteUInt32(inMax, outSize, outData, dev->GetClockDomain());
            case kAudioDevicePropertyLatency: return WriteUInt32(inMax, outSize, outData, dev->GetLatency());
            case kAudioDevicePropertySafetyOffset: return WriteUInt32(inMax, outSize, outData, dev->GetSafetyOffset());
            case kAudioDevicePropertyZeroTimeStampPeriod: return WriteUInt32(inMax, outSize, outData, dev->GetZeroTimestampPeriod());
            case kAudioDevicePropertyPreferredChannelLayout: {
                UInt32 size = dev->GetPreferredChannelLayoutSize(inAddress->mScope);
                if (outSize) *outSize = size;
                if (outData) { if (inMax < size) return kAudioHardwareBadPropertySizeError; memcpy(outData, dev->GetPreferredChannelLayout(inAddress->mScope), size); }
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
        case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats:
            if (outData && inMax < sizeof(AudioStreamRangedDescription)) return kAudioHardwareBadPropertySizeError;
            if (outData) *(AudioStreamRangedDescription*)outData = dev->GetStreamRangedDescription(isOutput);
            if (outSize) *outSize = sizeof(AudioStreamRangedDescription); return kAudioHardwareNoError;
        default: return kAudioHardwareUnknownPropertyError;
    }
}

extern "C" HRESULT OzzySetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, const void*) { return kAudioHardwareUnsupportedOperationError; }
extern "C" HRESULT OzzyStartIO(AudioServerPlugInDriverRef, AudioObjectID inDeviceObjectID, UInt32) {
    OzzyHALDevice* dev = OzzyHAL::Get().DeviceForObject(inDeviceObjectID);
    return dev ? dev->StartIO() : kAudioHardwareBadObjectError;
}
extern "C" HRESULT OzzyStopIO(AudioServerPlugInDriverRef, AudioObjectID inDeviceObjectID, UInt32) {
    OzzyHALDevice* dev = OzzyHAL::Get().DeviceForObject(inDeviceObjectID);
    return dev ? dev->StopIO() : kAudioHardwareBadObjectError;
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