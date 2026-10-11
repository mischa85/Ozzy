#ifndef OzzyHAL_h
#define OzzyHAL_h

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreAudioTypes/CoreAudioBaseTypes.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h> // Required for Kext Communication
#include <os/log.h>
#include <atomic>
#include <cstdint>
#include <pthread.h>
#include "../Shared/OzzySharedData.h"

// Codec function pointer types for device-agnostic encoding/decoding
typedef void (*WriteOutputFunc)(uint8_t* ringBuffer, const float* srcFrames, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize, uint32_t bytesPerFrame);
typedef void (*ReadInputFunc)(float* dstFrames, const uint8_t* ringBuffer, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize, uint32_t bytesPerFrame);
typedef void (*ClearOutputFunc)(uint8_t* outputBuffer, uint32_t bufferSize);

// One audio device per OzzyKext instance with audio. Device slot i owns three object IDs:
// the device (kOzzyObjectIDBase + 3i), its input stream (+1) and its output stream (+2).
// A slot keeps its IDs and UID when its device is replugged.
#define kOzzyMaxDevices         8
#define kOzzyObjectIDBase       100
#define kOzzyObjectsPerDevice   3

class OzzyHALDevice {
public:
    void Init(uint32_t slot);

    AudioObjectID GetDeviceID() const { return kOzzyObjectIDBase + mSlot * kOzzyObjectsPerDevice; }
    AudioObjectID GetInputStreamID() const { return GetDeviceID() + 1; }
    AudioObjectID GetOutputStreamID() const { return GetDeviceID() + 2; }

    // Connection to the kext (monitor thread)
    bool IsMapped() const { return mSHM != nullptr; }
    bool IsUsed() const { return mDeviceUID != nullptr; }   // has been published before
    uint64_t GetRegistryID() const { return mRegistryID; }
    bool Map(io_connect_t connect, mach_vm_address_t address, mach_vm_size_t size, uint64_t registryID, uint32_t locationID);
    void Unmap();
    bool CheckHealth();
    bool UpdateConnectionState(AudioServerPlugInHostRef host);   // true if it changed

    // CoreAudio state
    bool IsConnected() const { return mConnected; }
    bool IsRunning() const { return mIOStarted; }
    kern_return_t StartIO();
    kern_return_t StopIO();
    HRESULT GetZeroTimeStamp(Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed);
    OSStatus ioOperation(UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer);

    Float64 GetNominalSampleRate() const;
    AudioValueRange GetAvailableSampleRates() const;
    AudioBufferList* GetInputStreamConfiguration() const;
    AudioBufferList* GetOutputStreamConfiguration() const;
    CFStringRef GetDeviceName() const;
    CFStringRef GetManufacturer() const;
    CFStringRef GetDeviceUID() const;
    CFStringRef GetModelUID() const;
    UInt32 GetTransportType() const;
    UInt32 GetClockDomain() const;
    AudioStreamBasicDescription GetStreamFormat(bool isOutput) const;
    AudioStreamRangedDescription GetStreamRangedDescription(bool isOutput) const;

    UInt32 GetLatency() const { return 0; }
    UInt32 GetSafetyOffset() const { return 2 * mFramesPerPacket; }  // 2 URBs worth of frames
    UInt32 GetZeroTimestampPeriod() const { return kOzzyTimestampPackets * mFramesPerPacket; }  // spacing of the kext's timestamp anchors

    AudioChannelLayout* GetPreferredChannelLayout(AudioObjectPropertyScope inScope);
    UInt32 GetPreferredChannelLayoutSize(AudioObjectPropertyScope inScope) const;

    // The UID for a device (also used to find its old slot): its serial number, or for
    // devices without one (all Ploytec devices so far) its model and USB port
    static void MakeDeviceUID(const OzzySharedMemory* shm, uint32_t locationID, char* buf, size_t size);
    bool HasDeviceUID(const char* uid) const;

private:
    uint32_t mSlot = 0;

    CFStringRef mDeviceUID = nullptr;
    CFStringRef mModelUID = nullptr;
    CFStringRef mManufacturerUID = nullptr;

    uint8_t* mInputBufferAddr = nullptr;
    uint8_t* mOutputBufferAddr = nullptr;

    // Cached frame format for real-time I/O (updated on connect)
    uint32_t mFramesPerPacket = 80;
    uint32_t mRingSize = 10240; // kOzzyNumPackets * mFramesPerPacket
    uint32_t mOutputBytesPerFrame = 48;
    uint32_t mInputBytesPerFrame = 64;
    uint32_t mInputChannels = 8;    // published by the kext per model (16 for the WZ4)
    uint32_t mOutputChannels = 8;   // (2 for the WZ4)

    // Codecs picked once per device from what the kext published
    WriteOutputFunc mWriteOutput = nullptr;
    ReadInputFunc mReadInput = nullptr;
    ClearOutputFunc mClearOutput = nullptr;

    AudioStreamBasicDescription mCurrentStreamFormat;
    AudioBufferList mInputConfig;
    AudioBufferList mOutputConfig;
    AudioValueRange mAvailableSampleRates;

    bool mConnected = false;     // published to CoreAudio
    bool mIOStarted = false;
    uint32_t mTimestampSeed = 1;

    // Kext connection
    OzzySharedMemory* mSHM = nullptr;
    io_connect_t mKextConnect = IO_OBJECT_NULL;
    mach_vm_address_t mKextMapAddress = 0;
    mach_vm_size_t mKextMapSize = 0;
    uint64_t mRegistryID = 0;
    uint32_t mLocationID = 0;   // USB port (IOUSBHostDevice locationID)
    uint32_t mSessionID = 0;

    alignas(16) uint8_t mChannelLayoutBuffer[0x50C];

    void ApplyDeviceConfig();
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
