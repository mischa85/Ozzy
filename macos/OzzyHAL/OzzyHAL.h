#ifndef OzzyHAL_h
#define OzzyHAL_h

#include <mutex>
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreAudio/CoreAudioTypes.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h> // Required for Kext Communication
#include <os/log.h>
#include <atomic>
#include <cstdint>
#include <pthread.h>
#include "../../engine/shared/ozzy_shared_data.h"
#include "../../common/ozzy_devices.h"
#include "PloytecCodec.h"   // the codec function pointer types

/* The macOS 12 SDK renamed kAudioObjectPropertyElementMaster; older SDKs only have that name */
#ifndef __MAC_12_0
#define kAudioObjectPropertyElementMain kAudioObjectPropertyElementMaster
#endif

// One audio device per OzzyKext instance with audio. Device slot i owns four object IDs:
// the device (kOzzyObjectIDBase + 4i), its input stream (+1), its output stream (+2) and its
// clock source control (+3, published only for devices with the Clock Source setting).
// A slot keeps its IDs and UID when its device is replugged.
#define kOzzyMaxDevices         8
#define kOzzyObjectIDBase       100
#define kOzzyObjectsPerDevice   4

class OzzyHALDevice {
public:
    void Init(uint32_t slot);

    AudioObjectID GetDeviceID() const { return kOzzyObjectIDBase + mSlot * kOzzyObjectsPerDevice; }
    AudioObjectID GetInputStreamID() const { return GetDeviceID() + 1; }
    AudioObjectID GetOutputStreamID() const { return GetDeviceID() + 2; }
    AudioObjectID GetClockControlID() const { return GetDeviceID() + 3; }

    // Connection to the kext (monitor thread)
    bool IsMapped() const { return mSHM.load(std::memory_order_relaxed) != nullptr; }
    bool IsUsed() const { return mDeviceUID != nullptr; }   // has been published before
    uint64_t GetRegistryID() const { return mRegistryID; }
    bool Map(io_connect_t connect, mach_vm_address_t address, mach_vm_size_t size, uint64_t registryID, uint32_t locationID);
    void Unmap();
    bool CheckHealth();
    bool UpdateConnectionState(AudioServerPlugInHostRef host);   // true if it changed

    // CoreAudio state
    bool IsConnected() const { return mConnected; }
    bool IsRunning() const { return mIOStarted; }
    // Clients (AddDeviceClient / RemoveDeviceClient) and their IO. The device runs IO from the
    // first StartIO to the StopIO that balances it: AudioServerPlugIn.h has them come per client,
    // but coreaudiod (macOS 26) counts its clients itself, sending StartIO for the first and
    // StopIO, with the last one's ID, for the last; so the calls are counted, not matched by ID
    void AddClient(const AudioServerPlugInClientInfo* info);
    void RemoveClient(UInt32 clientID);
    kern_return_t StartIO(AudioServerPlugInHostRef host, UInt32 clientID);
    kern_return_t StopIO(AudioServerPlugInHostRef host, UInt32 clientID);
    // OZZY_PROPERTY_CLIENTS's data: a new CFArray of the clients doing IO
    CFArrayRef CopyClientsArray() const;
    HRESULT GetZeroTimeStamp(Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed);
    OSStatus ioOperation(UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer);

    Float64 GetNominalSampleRate() const;
    // The rates CoreAudio may pick (ozzy_device_host_rates), as ranges of one rate each, into
    // @out (NULL: just count); returns how many
    UInt32 CopySampleRates(AudioValueRange* out, UInt32 max) const;
    AudioBufferList* GetInputStreamConfiguration() const;
    AudioBufferList* GetOutputStreamConfiguration() const;
    CFStringRef GetDeviceName() const;
    CFStringRef GetManufacturer() const;
    CFStringRef GetDeviceUID() const;
    CFStringRef GetModelUID() const;
    UInt32 GetTransportType() const;
    UInt32 GetClockDomain() const;
    AudioStreamBasicDescription GetStreamFormat(bool isOutput) const;
    UInt32 CopyStreamRangedDescriptions(bool isOutput, AudioStreamRangedDescription* out, UInt32 max) const;
    // A client set the nominal rate or a stream format: one of the host rates asks CoreAudio
    // to stop IO and call PerformConfigurationChange
    OSStatus SetSampleRate(AudioServerPlugInHostRef host, Float64 rate);

    // Frames the device itself adds that way at the current rate (common/ozzy_devices.def)
    UInt32 GetLatency(AudioObjectPropertyScope scope) const;
    // Output: frames the engine reads ahead; input: frames behind the clock it is complete (ApplyDeviceConfig)
    UInt32 GetSafetyOffset(AudioObjectPropertyScope scope) const {
        return scope == kAudioObjectPropertyScopeInput ? mInputSafetyOffset : mSafetyOffset;
    }
    UInt32 GetZeroTimestampPeriod() const { return mTimestampFrames; }  // spacing of the kext's timestamp anchors

    // Device settings (common/ozzy_settings.h). The clock source is a clock source selector
    // control (kAudioClockSourceControlClassID), from which CoreAudio derives the device's
    // kAudioDevicePropertyClockSource (Audio MIDI Setup's Clock Source menu). Item IDs are
    // the setting's values (enum ozzy_clock_source).
    bool HasSetting(unsigned int id) const { return id < OZZY_NUM_SETTINGS && (mSettingsAvailable & (1u << id)); }
    bool HasSettings() const { return mSettingsAvailable != 0; }
    unsigned int SettingCount(unsigned int id) const;
    UInt32 GetSetting(unsigned int id) const { return mSettings.value[id]; }
    CFStringRef CopyClockSourceName(UInt32 item) const;
    // A client set a setting (OZZY_PROPERTY_SETTING, or the clock source control): a route
    // applies at once, a device setting asks CoreAudio to stop IO and call PerformSettingChange
    OSStatus SetSetting(AudioServerPlugInHostRef host, unsigned int id, UInt32 value);
    // OZZY_PROPERTY_SETTING's data: a new dictionary, and setting from one
    CFDictionaryRef CopySettingsDictionary() const;
    // OZZY_PROPERTY_INFO's data: a new CFData of the kext's struct ozzy_info
    CFDataRef CopyInfoData() const;
    // OZZY_PROPERTY_STATS's data: a new CFData of the kext's struct ozzy_stats, as it is now
    CFDataRef CopyStatsData();
    // The monitor thread: true once for a change in the stats' incidents (ozzy_stats_incidents)
    bool StatsChanged();
    // The custom properties the device has (kAudioObjectPropertyCustomPropertyInfoList): the
    // info, the stats, and the settings if it has any
    UInt32 CustomPropertyCount() const { return HasSettings() ? 4 : 3; }
    OSStatus SetSettingsDictionary(AudioServerPlugInHostRef host, CFDictionaryRef dict);
    // CoreAudio stopped IO for a setting or rate change: send it to the kext, re-read the
    // format, keep it
    OSStatus PerformConfigurationChange(AudioServerPlugInHostRef host, UInt64 action);

    AudioChannelLayout* GetPreferredChannelLayout(AudioObjectPropertyScope inScope);
    UInt32 GetPreferredChannelLayoutSize(AudioObjectPropertyScope inScope) const;

    // The UID for a device (also used to find its old slot): its serial number, or for
    // devices without one (all Ploytec devices so far) its model and USB port
    static void MakeDeviceUID(const ozzy_shared_memory* shm, uint32_t locationID, char* buf, size_t size);
    bool HasDeviceUID(const char* uid) const;

private:
    uint32_t mSlot = 0;

    CFStringRef mDeviceUID = nullptr;
    CFStringRef mModelUID = nullptr;
    CFStringRef mManufacturerUID = nullptr;

    // Cached frame format for real-time I/O (updated on connect)
    uint32_t mOutRingFrames = 1;   // frames in the shared memory's output ring (the engine's layout)
    uint32_t mInRingFrames = 1;
    uint32_t mTimestampFrames = OZZY_TIMESTAMP_FRAMES;   // between the engine's clock anchors
    uint32_t mSafetyOffset = 160; // frames the engine reads output ahead of the clock
    uint32_t mInputSafetyOffset = 160;   // frames behind the clock input is complete
    uint32_t mInputChannels = 8;    // published by the kext per model (16 for the WZ4)
    uint32_t mOutputChannels = 8;   // (2 for the WZ4)

    // Codecs picked once per device from what the kext published
    WriteOutputFunc mWriteOutput = nullptr;
    ReadInputFunc mReadInput = nullptr;
    ClearOutputFunc mClearOutput = nullptr;

    AudioStreamBasicDescription mCurrentStreamFormat;
    AudioBufferList mInputConfig;
    AudioBufferList mOutputConfig;
    uint32_t mHostRates[OZZY_MAX_RATES] = { 96000 };   // ApplyDeviceConfig
    uint32_t mHostRateCount = 1;
    const struct ozzy_device* mModel = nullptr;   // its row, null if not listed (ApplyDeviceConfig)

    // Settings as the kext applied them (ApplyDeviceConfig)
    uint32_t mSettingsAvailable = 0;   // bit (1 << enum ozzy_setting) per setting the device has
    struct ozzy_settings mSettings = {};
    bool mClockLocked = false;
    uint32_t mLockRate = 0;
    struct ozzy_info mInfo = {};        // the kext's, as of its last handshake (ApplyDeviceConfig)
    mutable std::mutex mInfoLock;       // ApplyDeviceConfig writes mInfo while a client may read it
    uint32_t mStatsIncidents = 0;       // as the monitor thread last told of them
    struct ozzy_route mInRoute = {};    // capture routing, applied after mReadInput
    struct ozzy_route mOutRoute = {};   // playback routing, applied before mWriteOutput
    bool mSpdifMirror = false;          // mWriteOutput is the ESU1808 mirror writer (UpdateRoutes)

    bool mConnected = false;     // published to CoreAudio
    bool mIOStarted = false;

    struct Client {
        UInt32 id;
        pid_t pid;
        char bundle[128];
        bool io;                 // started IO (where the host says so per client)
    };
    int mIOCount = 0;            // StartIOs not yet balanced by a StopIO (mClientLock)
    static constexpr int kMaxClients = 64;
    Client mClients[kMaxClients] = {};
    int mClientCount = 0;
    mutable std::mutex mClientLock;
    kern_return_t StartDeviceIO();
    void StopDeviceIO();
    void NotifyClients(AudioServerPlugInHostRef host);
    uint32_t mTimestampSeed = 1;

    // Kext connection. The monitor thread maps and unmaps; CoreAudio's threads use the
    // mapping between AcquireSHM and ReleaseSHM, and Unmap waits for them to finish.
    std::atomic<ozzy_shared_memory*> mSHM { nullptr };
    std::atomic<uint32_t> mSHMUsers { 0 };
    io_connect_t mKextConnect = IO_OBJECT_NULL;
    mach_vm_address_t mKextMapAddress = 0;
    mach_vm_size_t mKextMapSize = 0;
    uint64_t mRegistryID = 0;
    uint32_t mLocationID = 0;   // USB port (IOUSBHostDevice locationID)
    uint32_t mSessionID = 0;

    alignas(16) uint8_t mChannelLayoutBuffer[0x50C];

    ozzy_shared_memory* AcquireSHM();
    void ReleaseSHM();
    void ApplyDeviceConfig();
    kern_return_t CallSetRate(uint32_t rate);
    kern_return_t CallApplySetting(unsigned int id, uint32_t value);
    CFStringRef CopySettingKey(unsigned int id) const;
    void StoreSetting(AudioServerPlugInHostRef host, unsigned int id, uint32_t value);
    void RestoreSettings(AudioServerPlugInHostRef host);
    void UpdateRoutes();
    void UpdateMetadata();
    void RebuildStreamConfigs();
    void ClearOutputBuffer();
    void NotifyDeviceChanged(AudioServerPlugInHostRef host);
};

class OzzyHAL {
public:
    static OzzyHAL& Get();

    void Init(AudioServerPlugInHostRef host);
    void Cleanup();

    // Object ID lookup: the device that owns @objectID, or null
    OzzyHALDevice* DeviceForObject(AudioObjectID objectID);
    UInt32 GetDeviceList(AudioObjectID* outIDs, UInt32 maxIDs) const;
    AudioServerPlugInHostRef GetHost() const { return mHost; }

private:
    OzzyHAL() = default;
    ~OzzyHAL() = default;

    AudioServerPlugInHostRef mHost = nullptr;
    OzzyHALDevice mDevices[kOzzyMaxDevices];

    pthread_t mMonitorThread;
    std::atomic<bool> mMonitorRunning { false };
    bool mLoggedWaitingForDriver = false;

    static void* MonitorEntry(void* arg);
    void MonitorLoop();
    void ScanServices();
    void ConnectService(io_service_t service, uint64_t registryID);
    OzzyHALDevice* SlotForUID(const char* uid);
};

#endif
