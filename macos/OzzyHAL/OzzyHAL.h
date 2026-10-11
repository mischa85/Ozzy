#ifndef OzzyHAL_h
#define OzzyHAL_h

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreAudioTypes/CoreAudioBaseTypes.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h> // Required for Kext Communication
#include <os/log.h>
#include <atomic>
#include <cstdint>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include "../Shared/OzzySharedData.h"

// Codec function pointer types for device-agnostic encoding/decoding
typedef void (*WriteOutputFunc)(uint8_t* ringBuffer, const float* srcFrames, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize, uint32_t bytesPerFrame);
typedef void (*ReadInputFunc)(float* dstFrames, const uint8_t* ringBuffer, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize, uint32_t bytesPerFrame);
typedef void (*ClearOutputFunc)(uint8_t* outputBuffer, uint32_t bufferSize);

#define kOzzyDeviceID 100
#define kOzzyInputStreamID 150
#define kOzzyOutputStreamID 200

class OzzyHAL {
public:
    static OzzyHAL& Get();
    
    void Init(AudioServerPlugInHostRef host);
    void Cleanup();

    bool IsConnected();
    bool IsRunning() const { return mIOStarted; }

    kern_return_t StartIO();
    kern_return_t StopIO();

    HRESULT GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed);
    
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
    AudioObjectID GetInputStreamID() const;
    AudioObjectID GetOutputStreamID() const;
    AudioStreamBasicDescription GetStreamFormat(bool isOutput) const;
    AudioStreamRangedDescription GetStreamRangedDescription(bool isOutput) const;
    
    UInt32 GetLatency() const { return 0; }
    UInt32 GetSafetyOffset() const { return 2 * mFramesPerPacket; }  // 2 URBs worth of frames
    UInt32 GetZeroTimestampPeriod() const { return kOzzyTimestampPackets * mFramesPerPacket; }  // spacing of the kext's timestamp anchors
    
    AudioChannelLayout* GetPreferredChannelLayout(AudioObjectPropertyScope inScope);
    UInt32 GetPreferredChannelLayoutSize(AudioObjectPropertyScope inScope) const;
    
    OSStatus ioOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer);

private:
    OzzyHAL() = default;
    ~OzzyHAL() = default;

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
    
    // Codec function pointers (set based on device vendor/product ID)
    WriteOutputFunc mWriteOutput = nullptr;
    ReadInputFunc mReadInput = nullptr;
    ClearOutputFunc mClearOutput = nullptr;
    
    AudioStreamBasicDescription mCurrentStreamFormat;
    AudioBufferList mInputConfig;
    AudioBufferList mOutputConfig;
    AudioValueRange mAvailableSampleRates;
    
    bool mIOStarted = false;
    uint32_t mTimestampSeed = 1;

    AudioServerPlugInHostRef mHost = nullptr;
    OzzySharedMemory* mSHM = nullptr;
    
    // --- Kext Support ---
    io_connect_t mKextConnect = IO_OBJECT_NULL;
    mach_vm_address_t mKextMapAddress = 0;
    mach_vm_size_t mKextMapSize = 0;
    bool mIsKextMapping = false;

    // --- Legacy Daemon Support ---
    int mSHMFd = -1;
    ino_t mSHMInode = 0;

    uint32_t mLastSessionID = 0;

    pthread_t mMonitorThread;
    std::atomic<bool> mMonitorRunning { false };
    bool mLastConnectedState = false;
    bool mLoggedWaitingForDriver = false;
    alignas(16) uint8_t mChannelLayoutBuffer[0x50C];

    void MapSharedMemory();
    void ApplyDeviceConfig();
    void UnmapSharedMemory();
    static void* MonitorEntry(void* arg);
    void MonitorLoop();
    void RebuildStreamConfigs();

    void ClearOutputBuffer();
};

#endif