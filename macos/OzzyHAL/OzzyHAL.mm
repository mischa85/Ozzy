#include "OzzyHAL.h"
#include "../Devices/Ploytec/PloytecCodec.h"
#include "../Devices/Ploytec/PloytecProtocol.h"
#include "../Shared/OzzyLog.h"
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

OzzyHAL& OzzyHAL::Get() {
    static OzzyHAL instance;
    return instance;
}

void OzzyHAL::Init(AudioServerPlugInHostRef host) {
    LogOzzyHAL("init");
    mHost = host;

    mDeviceUID = CFSTR("OzzyHAL.device");
    mModelUID = CFSTR("Ozzy Audio Device");
    mManufacturerUID = CFSTR("Ozzy");
    mZeroTimestampPeriod = 640;

    mCurrentStreamFormat.mSampleRate = 48000.0;
    mCurrentStreamFormat.mFormatID = kAudioFormatLinearPCM;
    mCurrentStreamFormat.mFormatFlags = kAudioFormatFlagIsFloat;
    mCurrentStreamFormat.mBytesPerPacket = 32;
    mCurrentStreamFormat.mFramesPerPacket = 1;
    mCurrentStreamFormat.mBytesPerFrame = 32;
    mCurrentStreamFormat.mChannelsPerFrame = 8;
    mCurrentStreamFormat.mBitsPerChannel = 32;

    mAvailableSampleRates.mMinimum = 48000.0;
    mAvailableSampleRates.mMaximum = 48000.0;

    RebuildStreamConfigs();

    // Try to map shared memory immediately to detect already-connected devices
    MapSharedMemory();
    
    // Start monitoring thread for connection changes
    mMonitorRunning = true;
    pthread_create(&mMonitorThread, NULL, MonitorEntry, this);
}

void OzzyHAL::Cleanup() {
    mMonitorRunning = false;
    pthread_join(mMonitorThread, NULL);
    UnmapSharedMemory();
}

void OzzyHAL::ClearOutputBuffer() {
    if (!mOutputBufferAddr || !mSHM || !mClearOutput) return;
    const uint32_t bufferSize = kOzzyMaxPacketSize * kOzzyNumPackets;
    mClearOutput(mOutputBufferAddr, bufferSize);
}

void OzzyHAL::RebuildStreamConfigs() {
    const uint32_t kDefaultIOFrames = 512;
    const uint32_t kBytesPerFrame = mCurrentStreamFormat.mBytesPerFrame;

    mInputConfig.mNumberBuffers = 1;
    mInputConfig.mBuffers[0].mNumberChannels = 8;
    mInputConfig.mBuffers[0].mDataByteSize = kDefaultIOFrames * kBytesPerFrame;
    mInputConfig.mBuffers[0].mData = nullptr;

    mOutputConfig.mNumberBuffers = 1;
    mOutputConfig.mBuffers[0].mNumberChannels = 8;
    mOutputConfig.mBuffers[0].mDataByteSize = kDefaultIOFrames * kBytesPerFrame;
    mOutputConfig.mBuffers[0].mData = nullptr;
}

void OzzyHAL::MapSharedMemory() {
    if (mSHM) return;
    
    // Close any stale kext connection first
    if (mKextConnect != IO_OBJECT_NULL) {
        IOServiceClose(mKextConnect);
        mKextConnect = IO_OBJECT_NULL;
        mKextMapAddress = 0;
        mKextMapSize = 0;
    }

    // --- 1. Try OzzyKext (IOKit) ---
    CFMutableDictionaryRef matching = IOServiceMatching("OzzyKext");
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, matching);
    
    if (service) {
        LogOzzyHAL("found OzzyKext service, connecting...");
        kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &mKextConnect);
        IOObjectRelease(service);

        if (kr == KERN_SUCCESS) {
            kr = IOConnectMapMemory(mKextConnect, 0, mach_task_self(), &mKextMapAddress, &mKextMapSize, kIOMapAnywhere);
            if (kr == KERN_SUCCESS) {
                mSHM = (OzzySharedMemory*)mKextMapAddress;
                mIsKextMapping = true;
                LogOzzyHAL("attached to kext (session: 0x%{public}08X)", mSHM->sessionID);
            } else {
                LogOzzyHALError("failed to map kext memory (0x%{public}x)", kr);
                IOServiceClose(mKextConnect);
                mKextConnect = IO_OBJECT_NULL;
            }
        }
    }

    // --- 2. Fallback to Daemon (shm_open) ---
    if (!mSHM) {
        mIsKextMapping = false;
        mSHMFd = shm_open(kOzzySharedMemName, O_RDWR, 0666);
        if (mSHMFd != -1) {
            struct stat sb;
            if (fstat(mSHMFd, &sb) == 0) {
                mSHMInode = sb.st_ino;
                void* ptr = mmap(NULL, sizeof(OzzySharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, mSHMFd, 0);
                if (ptr != MAP_FAILED) {
                    mSHM = (OzzySharedMemory*)ptr;
                    LogOzzyHAL("attached to daemon via shm_open");
                } else {
                    close(mSHMFd); mSHMFd = -1;
                }
            } else {
                close(mSHMFd); mSHMFd = -1;
            }
        }
    }

    // --- Validation & Config ---
    if (mSHM) {
        if (mSHM->magic != kOzzyMagic) {
            LogOzzyHALError("SHM magic mismatch");
            UnmapSharedMemory();
            return;
        }

        if (mSHM->vendorID == 0) {
            UnmapSharedMemory();
            return;
        }

        mLastSessionID = mSHM->sessionID;
        mInputBufferAddr = mSHM->audio.inputBuffer;
        mOutputBufferAddr = mSHM->audio.outputBuffer;
        
        // Cache frame format for real-time I/O performance
        mFramesPerPacket = mSHM->audio.framesPerPacket;  // e.g., 80 frames per logical packet
        // Ring buffer size in FRAMES: 128 logical packets * 80 frames = 10,240 frames
        mRingSize = kOzzyNumPackets * mFramesPerPacket;
        mOutputBytesPerFrame = mSHM->audio.outputBytesPerFrame;
        mInputBytesPerFrame = mSHM->audio.inputBytesPerFrame;
        
        // Calculate zero timestamp period: 2 URBs * framesPerPacket * 4
        mZeroTimestampPeriod = 2 * mFramesPerPacket * 4;
        
        // Set codec based on device (vendorID/productID)
        bool isBulk = (mSHM->audio.deviceFlags & 1);
        if (mSHM->vendorID == PLOYTEC_VENDOR_ID) {
            mEncode = PloytecEncodePCM;
            mDecode = PloytecDecodePCM;
            mWriteOutput = isBulk ? PloytecWriteOutputBulk : PloytecWriteOutputInterrupt;
            mReadInput = PloytecReadInput;
            mClearOutput = isBulk ? PloytecClearOutputBulk : PloytecClearOutputInterrupt;
            LogOzzyHAL("using Ploytec codec (%{public}s) for VID:0x%{public}04X",
                       isBulk ? "bulk" : "interrupt", mSHM->vendorID);
        } else {
            mEncode = PloytecEncodePCM;
            mDecode = PloytecDecodePCM;
            mWriteOutput = isBulk ? PloytecWriteOutputBulk : PloytecWriteOutputInterrupt;
            mReadInput = PloytecReadInput;
            mClearOutput = isBulk ? PloytecClearOutputBulk : PloytecClearOutputInterrupt;
            LogOzzyHAL("using default codec (%{public}s) for VID:0x%{public}04X",
                       isBulk ? "bulk" : "interrupt", mSHM->vendorID);
        }
    }
}

void OzzyHAL::UnmapSharedMemory() {
    if (!mSHM) return;

    if (mIsKextMapping) {
        // Cleanup Kext
        if (mKextMapAddress) {
            IOConnectUnmapMemory(mKextConnect, 0, mach_task_self(), mKextMapAddress);
            mKextMapAddress = 0;
        }
        if (mKextConnect != IO_OBJECT_NULL) {
            IOServiceClose(mKextConnect);
            mKextConnect = IO_OBJECT_NULL;
        }
    } else {
        // Cleanup Daemon
        munmap(mSHM, sizeof(OzzySharedMemory));
        if (mSHMFd != -1) { close(mSHMFd); mSHMFd = -1; }
        mSHMInode = 0;
    }

    mSHM = nullptr;
    mInputBufferAddr = nullptr;
    mOutputBufferAddr = nullptr;
    mIsKextMapping = false;
}

void* OzzyHAL::MonitorEntry(void* arg) {
    ((OzzyHAL*)arg)->MonitorLoop();
    return NULL;
}

void OzzyHAL::MonitorLoop() {
    // Check initial state if already mapped
    if (mSHM) {
        bool hwReady = mSHM->audio.hardwarePresent.load();
        bool drvReady = mSHM->audio.driverReady.load();
        bool metaReady = (mSHM->productName[0] != 0);
        bool initiallyConnected = (hwReady && drvReady && metaReady);
        
        if (initiallyConnected) {
            LogOzzyHAL("device already connected at startup");
            mLastConnectedState = true;
            
            // Update device metadata
            if (mSHM->productName[0] != 0) {
                if (mModelUID) CFRelease(mModelUID);
                mModelUID = SafeCreateString(mSHM->productName);
            }
            if (mSHM->manufacturerName[0] != 0) {
                if (mManufacturerUID) CFRelease(mManufacturerUID);
                mManufacturerUID = SafeCreateString(mSHM->manufacturerName);
            }
            if (mSHM->serialNumber[0] != 0) {
                if (mDeviceUID) CFRelease(mDeviceUID);
                char uidBuf[128];
                snprintf(uidBuf, sizeof(uidBuf), "OzzyHAL.%s", mSHM->serialNumber);
                mDeviceUID = SafeCreateString(uidBuf);
            }
            
            // Notify CoreAudio
            if (mHost) {
                AudioObjectPropertyAddress addr = { kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
                mHost->PropertiesChanged(mHost, kAudioObjectPlugInObject, 1, &addr);
            }
        }
    }
    
    while (mMonitorRunning) {
        if (!mSHM) {
            if (!mLoggedWaitingForDriver) {
                LogOzzyHAL("waiting for driver...");
                mLoggedWaitingForDriver = true;
            }
            MapSharedMemory();
            if (!mSHM) {
                usleep(100000); // 100ms
                continue;
            }
            mLoggedWaitingForDriver = false;
        } else {
            // Check Health
            if (mIsKextMapping) {
                // Check if shared memory is still valid and hardware is present
                bool magicValid = (mSHM->magic == kOzzyMagic);
                bool hwPresent = magicValid && mSHM->audio.hardwarePresent.load(std::memory_order_relaxed);
                
                if (!magicValid || !hwPresent) {
                     LogOzzyHAL("device disconnected (magic: %{public}d, hw: %{public}d)", magicValid, hwPresent);
                     UnmapSharedMemory();
                     // Don't continue - let state update happen below
                }
            } else {
                // If using Daemon, check inode and hardware presence
                struct stat sb;
                if (fstat(mSHMFd, &sb) != 0 || sb.st_nlink == 0 || sb.st_ino != mSHMInode) {
                    LogOzzyHALError("shared memory unlinked (daemon died)");
                    UnmapSharedMemory();
                    // Don't continue - let state update happen below
                } else {
                    // Also check if hardware is still present
                    bool magicValid = (mSHM->magic == kOzzyMagic);
                    bool hwPresent = magicValid && mSHM->audio.hardwarePresent.load(std::memory_order_relaxed);

                    if (!magicValid || !hwPresent) {
                        LogOzzyHAL("device disconnected (magic: %{public}d, hw: %{public}d)", magicValid, hwPresent);
                        UnmapSharedMemory();
                        // Don't continue - let state update happen below
                    }
                }
            }
        }

        bool hwReady = (mSHM && mSHM->audio.hardwarePresent.load());
        bool drvReady = (mSHM && mSHM->audio.driverReady.load());
        bool metaReady = (mSHM && mSHM->productName[0] != 0);
        bool currentlyConnected = (hwReady && drvReady && metaReady);

        if (currentlyConnected != mLastConnectedState) {
            if (currentlyConnected) {
                if (mSHM->productName[0] != 0) {
                    if (mModelUID) CFRelease(mModelUID);
                    mModelUID = SafeCreateString(mSHM->productName);
                }
                if (mSHM->manufacturerName[0] != 0) {
                    if (mManufacturerUID) CFRelease(mManufacturerUID);
                    mManufacturerUID = SafeCreateString(mSHM->manufacturerName);
                }
                if (mSHM->serialNumber[0] != 0) {
                    if (mDeviceUID) CFRelease(mDeviceUID);
                    char uidBuf[128];
                    snprintf(uidBuf, sizeof(uidBuf), "OzzyHAL.%s", mSHM->serialNumber);
                    mDeviceUID = SafeCreateString(uidBuf);
                }
                LogOzzyHAL("device ready: '%{public}s'", mSHM->productName);
            }

            mLastConnectedState = currentlyConnected;
            LogOzzyHAL("connection: %{public}s", currentlyConnected ? "UP" : "DOWN");
            
            // Stop I/O if device disconnected
            if (!currentlyConnected && mIOStarted) {
                mIOStarted = false;
                mTimestampSeed++;
                LogOzzyHAL("auto-stopped I/O due to disconnect");
            }
            
            if (mHost) {
                if (currentlyConnected) {
                    // Device connected - notify about device properties first, then add to list
                    AudioObjectPropertyAddress devAddr = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
                    mHost->PropertiesChanged(mHost, kOzzyDeviceID, 1, &devAddr);
                    devAddr.mSelector = kAudioObjectPropertyManufacturer;
                    mHost->PropertiesChanged(mHost, kOzzyDeviceID, 1, &devAddr);
                    devAddr.mSelector = kAudioDevicePropertyDeviceUID;
                    mHost->PropertiesChanged(mHost, kOzzyDeviceID, 1, &devAddr);
                    
                    AudioObjectPropertyAddress listAddr = { kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
                    mHost->PropertiesChanged(mHost, kAudioObjectPlugInObject, 1, &listAddr);
                } else {
                    // Device disconnected - notify that device is no longer alive, then remove from list
                    AudioObjectPropertyAddress aliveAddr = { kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
                    mHost->PropertiesChanged(mHost, kOzzyDeviceID, 1, &aliveAddr);
                    
                    AudioObjectPropertyAddress listAddr = { kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
                    mHost->PropertiesChanged(mHost, kAudioObjectPlugInObject, 1, &listAddr);
                }
            }
        }
        usleep(250000);
    }
}

bool OzzyHAL::IsConnected() { return mLastConnectedState; }

kern_return_t OzzyHAL::StartIO() {
    if (!IsConnected()) return kAudioHardwareNotRunningError;
    if (mSHM) {
        mSHM->audio.halWritePosition.store(0, std::memory_order_relaxed);
    }
    LogOzzyHAL("start I/O");
    mIOStarted = true;
    mTimestampSeed++;
    return kIOReturnSuccess;
}

kern_return_t OzzyHAL::StopIO() {
    LogOzzyHAL("stop I/O");
    mIOStarted = false;
    mTimestampSeed++;
    ClearOutputBuffer();
    return kIOReturnSuccess;
}

HRESULT OzzyHAL::GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed) {
    if (!mSHM) return kAudioHardwareNotRunningError;

    auto& ts = mSHM->audio.timestamp;
    uint64_t sTime, hTime;
    uint32_t seq1, seq2;
    int retries = 0;

    do {
        seq1 = ts.sequence.load(std::memory_order_acquire);
        if (seq1 & 1) {
            std::atomic_thread_fence(std::memory_order_acquire);
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

OSStatus OzzyHAL::ioOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer) {
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

CFStringRef OzzyHAL::GetDeviceName() const { return mModelUID; }
CFStringRef OzzyHAL::GetManufacturer() const { return mManufacturerUID; }
CFStringRef OzzyHAL::GetDeviceUID() const { return mDeviceUID; }
CFStringRef OzzyHAL::GetModelUID() const { return mModelUID; }
UInt32 OzzyHAL::GetTransportType() const { return kAudioDeviceTransportTypeUSB; }
UInt32 OzzyHAL::GetClockDomain() const { return 0x504C4F59; }
Float64 OzzyHAL::GetNominalSampleRate() const { return 48000.0; }
AudioValueRange OzzyHAL::GetAvailableSampleRates() const { return mAvailableSampleRates; }
AudioBufferList* OzzyHAL::GetInputStreamConfiguration() const { return const_cast<AudioBufferList*>(&mInputConfig); }
AudioBufferList* OzzyHAL::GetOutputStreamConfiguration() const { return const_cast<AudioBufferList*>(&mOutputConfig); }
AudioObjectID OzzyHAL::GetInputStreamID() const { return kOzzyInputStreamID; }
AudioObjectID OzzyHAL::GetOutputStreamID() const { return kOzzyOutputStreamID; }
AudioStreamBasicDescription OzzyHAL::GetStreamFormat() const { return mCurrentStreamFormat; }
AudioStreamRangedDescription OzzyHAL::GetStreamRangedDescription() const {
    AudioStreamRangedDescription outDesc; outDesc.mFormat = GetStreamFormat();
    outDesc.mSampleRateRange.mMinimum = 48000.0; outDesc.mSampleRateRange.mMaximum = 48000.0;
    return outDesc;
}
UInt32 OzzyHAL::GetZeroTimestampPeriod() const { return mZeroTimestampPeriod; }

AudioChannelLayout* OzzyHAL::GetPreferredChannelLayout(AudioObjectPropertyScope inScope) {
    AudioChannelLayout* layout = (AudioChannelLayout*)mChannelLayoutBuffer;
    /* MPEG_7_1_A: L, R, C, LFE, Ls, Rs, Lss, Rss (8 channels with standard L/R labels).
     * CoreAudio maps stereo afplay (L→ch0, R→ch1) because the first two channels carry
     * kAudioChannelLabel_Left and kAudioChannelLabel_Right.
     * DiscreteInOrder would assign kAudioChannelLabel_Discrete_0..7 which never matches
     * any stereo source label, causing CoreAudio to silence the entire mix. */
    layout->mChannelLayoutTag = kAudioChannelLayoutTag_MPEG_7_1_A;
    layout->mChannelBitmap = 0;
    layout->mNumberChannelDescriptions = 0;
    return layout;
}

UInt32 OzzyHAL::GetPreferredChannelLayoutSize(AudioObjectPropertyScope inScope) const {
    return offsetof(AudioChannelLayout, mChannelDescriptions); /* no descriptions when using tag */
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
    if (inObjectID == kOzzyDeviceID) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass:
            case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer:
            case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID:
            case kAudioDevicePropertyTransportType: case kAudioDevicePropertyIsHidden:
            case kAudioDevicePropertyDeviceIsAlive: case kAudioDevicePropertyDeviceIsRunning:
            case kAudioDevicePropertyDeviceIsRunningSomewhere: case kAudioDevicePropertyNominalSampleRate:
            case kAudioDevicePropertyAvailableNominalSampleRates: case kAudioDevicePropertyStreams:
            case kAudioDevicePropertyStreamConfiguration: case kAudioDevicePropertyLatency:
            case kAudioDevicePropertySafetyOffset: case kAudioDevicePropertyPreferredChannelLayout:
            case kAudioDevicePropertyZeroTimeStampPeriod: return true;
        }
        return false;
    }
    if (inObjectID == kOzzyOutputStreamID || inObjectID == kOzzyInputStreamID) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass:
            case kAudioObjectPropertyName: case kAudioObjectPropertyOwner:
            case kAudioStreamPropertyDirection: case kAudioStreamPropertyTerminalType:
            case kAudioStreamPropertyStartingChannel: case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat: case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats: return true;
        }
        return false;
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
                *outDataSize = OzzyHAL::Get().IsConnected() ? sizeof(AudioObjectID) : 0; return kAudioHardwareNoError;
            case kAudioObjectPropertyControlList: *outDataSize = 0; return kAudioHardwareNoError;
            case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer: *outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
            case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
            default: return kAudioHardwareUnknownPropertyError;
        }
    }
    if (inObjectID == kOzzyDeviceID) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer:
            case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID:
                *outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
            case kAudioObjectPropertyControlList: *outDataSize = 0; return kAudioHardwareNoError;
            case kAudioObjectPropertyOwner: *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
            case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
            case kAudioDevicePropertyTransportType: case kAudioDevicePropertyIsHidden:
            case kAudioDevicePropertyDeviceIsAlive: case kAudioDevicePropertyDeviceIsRunning:
            case kAudioDevicePropertyDeviceIsRunningSomewhere: case kAudioDevicePropertyNominalSampleRate:
                *outDataSize = sizeof(Float64); return kAudioHardwareNoError;
            case kAudioDevicePropertyAvailableNominalSampleRates: *outDataSize = sizeof(AudioValueRange); return kAudioHardwareNoError;
            case kAudioDevicePropertyStreams:
                if (inAddress->mScope == kAudioObjectPropertyScopeInput) *outDataSize = sizeof(AudioObjectID);
                else if (IsOutputScope(inAddress->mScope)) *outDataSize = sizeof(AudioObjectID);
                else if (inAddress->mScope == kAudioObjectPropertyScopeGlobal) *outDataSize = 2 * sizeof(AudioObjectID);
                else *outDataSize = 0;
                return kAudioHardwareNoError;
            case kAudioDevicePropertyStreamConfiguration:
                *outDataSize = GetAudioBufferListSize((IsOutputScope(inAddress->mScope) ? OzzyHAL::Get().GetOutputStreamConfiguration() : OzzyHAL::Get().GetInputStreamConfiguration())->mNumberBuffers);
                return kAudioHardwareNoError;
            case kAudioDevicePropertySafetyOffset: case kAudioDevicePropertyLatency: case kAudioDevicePropertyZeroTimeStampPeriod: *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
            case kAudioDevicePropertyPreferredChannelLayout: *outDataSize = OzzyHAL::Get().GetPreferredChannelLayoutSize(inAddress->mScope); return kAudioHardwareNoError;
            default: return kAudioHardwareUnknownPropertyError;
        }
    }
    if (inObjectID == kOzzyOutputStreamID || inObjectID == kOzzyInputStreamID) {
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
    return kAudioHardwareBadObjectError;
}

extern "C" HRESULT OzzyGetPropertyData(AudioServerPlugInDriverRef, AudioObjectID inObjectID, pid_t, const AudioObjectPropertyAddress* inAddress, UInt32, const void*, UInt32 inMax, UInt32* outSize, void* outData) {
    if (inObjectID == kAudioObjectPlugInObject) {
        switch (inAddress->mSelector) {
            case kAudioPlugInPropertyDeviceList: case kAudioObjectPropertyOwnedObjects:
                if (!OzzyHAL::Get().IsConnected()) { if (outSize) *outSize = 0; return kAudioHardwareNoError; }
                return WriteObjectID(inMax, outSize, outData, kOzzyDeviceID);
            case kAudioObjectPropertyControlList: if (outSize) *outSize = 0; return kAudioHardwareNoError;
            case kAudioObjectPropertyName: return WriteCFString(inMax, outSize, outData, CFSTR("Ozzy HAL"));
            case kAudioObjectPropertyManufacturer: return WriteCFString(inMax, outSize, outData, CFSTR("Ozzy"));
            case kAudioObjectPropertyBaseClass: return WriteClassID(inMax, outSize, outData, kAudioObjectClassID);
            case kAudioObjectPropertyClass: return WriteClassID(inMax, outSize, outData, kAudioPlugInClassID);
            default: return kAudioHardwareUnknownPropertyError;
        }
    }
    if (inObjectID == kOzzyDeviceID) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass: return WriteClassID(inMax, outSize, outData, kAudioObjectClassID);
            case kAudioObjectPropertyClass: return WriteClassID(inMax, outSize, outData, kAudioDeviceClassID);
            case kAudioObjectPropertyControlList: if (outSize) *outSize = 0; return kAudioHardwareNoError;
            case kAudioObjectPropertyOwner: return WriteObjectID(inMax, outSize, outData, kAudioObjectPlugInObject);
            case kAudioObjectPropertyName: return WriteCFString(inMax, outSize, outData, OzzyHAL::Get().GetDeviceName());
            case kAudioObjectPropertyManufacturer: return WriteCFString(inMax, outSize, outData, OzzyHAL::Get().GetManufacturer());
            case kAudioDevicePropertyDeviceUID: return WriteCFString(inMax, outSize, outData, OzzyHAL::Get().GetDeviceUID());
            case kAudioDevicePropertyModelUID: return WriteCFString(inMax, outSize, outData, OzzyHAL::Get().GetModelUID());
            case kAudioDevicePropertyTransportType: return WriteUInt32(inMax, outSize, outData, OzzyHAL::Get().GetTransportType());
            case kAudioDevicePropertyIsHidden: return WriteUInt32(inMax, outSize, outData, 0);
            case kAudioDevicePropertyDeviceIsAlive: return WriteUInt32(inMax, outSize, outData, OzzyHAL::Get().IsConnected() ? 1 : 0);
            case kAudioDevicePropertyDeviceIsRunning: case kAudioDevicePropertyDeviceIsRunningSomewhere: return WriteUInt32(inMax, outSize, outData, OzzyHAL::Get().IsRunning() ? 1 : 0);
            case kAudioDevicePropertyNominalSampleRate: return WriteFloat64(inMax, outSize, outData, OzzyHAL::Get().GetNominalSampleRate());
            case kAudioDevicePropertyAvailableNominalSampleRates: if (outSize) *outSize = sizeof(AudioValueRange); if (outData) { if (inMax < sizeof(AudioValueRange)) return kAudioHardwareBadPropertySizeError; *(AudioValueRange*)outData = OzzyHAL::Get().GetAvailableSampleRates(); } return kAudioHardwareNoError;
            case kAudioDevicePropertyStreams: {
                UInt32 streamList[] = { kOzzyInputStreamID, kOzzyOutputStreamID };
                if (inAddress->mScope == kAudioObjectPropertyScopeInput) return WriteObjectID(inMax, outSize, outData, kOzzyInputStreamID);
                else if (inAddress->mScope == kAudioObjectPropertyScopeOutput) return WriteObjectID(inMax, outSize, outData, kOzzyOutputStreamID);
                else { if (outData && inMax < sizeof(streamList)) return kAudioHardwareBadPropertySizeError; if (outData) memcpy(outData, streamList, sizeof(streamList)); if (outSize) *outSize = sizeof(streamList); return kAudioHardwareNoError; }
            }
            case kAudioObjectPropertyOwnedObjects: {
                if (inMax < 2 * sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                AudioObjectID* list = (AudioObjectID*)outData; if (list) { list[0] = kOzzyOutputStreamID; list[1] = kOzzyInputStreamID; }
                if (outSize) *outSize = 2 * sizeof(AudioObjectID); return kAudioHardwareNoError;
            }
            case kAudioDevicePropertyStreamConfiguration: {
                AudioBufferList* config = (inAddress->mScope == kAudioObjectPropertyScopeInput) ? OzzyHAL::Get().GetInputStreamConfiguration() : OzzyHAL::Get().GetOutputStreamConfiguration();
                UInt32 actualSize = GetAudioBufferListSize(config->mNumberBuffers); if (outSize) *outSize = actualSize;
                if (outData) { if (inMax < actualSize) return kAudioHardwareBadPropertySizeError; memcpy(outData, config, actualSize); } return kAudioHardwareNoError;
            }
            case kAudioDevicePropertyClockDomain: return WriteUInt32(inMax, outSize, outData, OzzyHAL::Get().GetClockDomain());
            case kAudioDevicePropertyLatency: return WriteUInt32(inMax, outSize, outData, OzzyHAL::Get().GetLatency());
            case kAudioDevicePropertySafetyOffset: return WriteUInt32(inMax, outSize, outData, OzzyHAL::Get().GetSafetyOffset());
            case kAudioDevicePropertyZeroTimeStampPeriod: return WriteUInt32(inMax, outSize, outData, OzzyHAL::Get().GetZeroTimestampPeriod());
            case kAudioDevicePropertyPreferredChannelLayout: {
                UInt32 size = OzzyHAL::Get().GetPreferredChannelLayoutSize(inAddress->mScope);
                if (outSize) *outSize = size;
                if (outData) { if (inMax < size) return kAudioHardwareBadPropertySizeError; memcpy(outData, OzzyHAL::Get().GetPreferredChannelLayout(inAddress->mScope), size); }
                return kAudioHardwareNoError;
            }
            default: return kAudioHardwareUnknownPropertyError;
        }
    }
    if (inObjectID == kOzzyOutputStreamID || inObjectID == kOzzyInputStreamID) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass: return WriteClassID(inMax, outSize, outData, kAudioObjectClassID);
            case kAudioObjectPropertyClass: return WriteClassID(inMax, outSize, outData, kAudioStreamClassID);
            case kAudioObjectPropertyName: return WriteCFString(inMax, outSize, outData, (inObjectID == kOzzyOutputStreamID ? CFSTR("Ozzy PCM Output Stream") : CFSTR("Ozzy PCM Input Stream")));
            case kAudioObjectPropertyOwner: return WriteObjectID(inMax, outSize, outData, kOzzyDeviceID);
            case kAudioStreamPropertyDirection: return WriteUInt32(inMax, outSize, outData, (inObjectID == kOzzyOutputStreamID ? 0 : 1));
            case kAudioStreamPropertyTerminalType: return WriteUInt32(inMax, outSize, outData, kAudioStreamTerminalTypeSpeaker);
            case kAudioStreamPropertyStartingChannel: return WriteUInt32(inMax, outSize, outData, 1);
            case kAudioStreamPropertyLatency: return WriteUInt32(inMax, outSize, outData, OzzyHAL::Get().GetLatency());
            case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat:
                if (outData && inMax < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
                if (outData) *(AudioStreamBasicDescription*)outData = OzzyHAL::Get().GetStreamFormat();
                if (outSize) *outSize = sizeof(AudioStreamBasicDescription); return kAudioHardwareNoError;
            case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats:
                if (outData && inMax < sizeof(AudioStreamRangedDescription)) return kAudioHardwareBadPropertySizeError;
                if (outData) *(AudioStreamRangedDescription*)outData = OzzyHAL::Get().GetStreamRangedDescription();
                if (outSize) *outSize = sizeof(AudioStreamRangedDescription); return kAudioHardwareNoError;
            default: return kAudioHardwareUnknownPropertyError;
        }
    }
    return kAudioHardwareBadObjectError;
}

extern "C" HRESULT OzzySetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, const void*) { return kAudioHardwareUnsupportedOperationError; }
extern "C" HRESULT OzzyStartIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32) { return OzzyHAL::Get().StartIO(); }
extern "C" HRESULT OzzyStopIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32) { return OzzyHAL::Get().StopIO(); }
extern "C" HRESULT OzzyGetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed) { return OzzyHAL::Get().GetZeroTimeStamp(inDriver, inDeviceObjectID, inClientID, outSampleTime, outHostTime, outSeed); }
extern "C" OSStatus OzzyWillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32 inOperationID, Boolean* outWillDo, Boolean* outWillDoInPlace) { bool willDo = false; bool willDoInPlace = true; switch (inOperationID) { case kAudioServerPlugInIOOperationWriteMix: willDo = true; willDoInPlace = false; break; case kAudioServerPlugInIOOperationReadInput: willDo = true; break; default: willDo = false; break; } if (outWillDo) *outWillDo = willDo; if (outWillDoInPlace) *outWillDoInPlace = willDoInPlace; return kAudioHardwareNoError; }
extern "C" OSStatus OzzyBeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*) { return 0; }
extern "C" OSStatus OzzyDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer) { return OzzyHAL::Get().ioOperation(inDriver, inDeviceObjectID, inStreamObjectID, inClientID, inOperationID, inIOBufferFrameSize, inIOCycleInfo, ioMainBuffer, ioSecondaryBuffer); }
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