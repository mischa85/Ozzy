#ifndef OzzySharedData_h
#define OzzySharedData_h

#include <stdint.h>
#include "../../common/ozzy_settings.h"

// --- KERNEL vs USER SPACE SHIM ---
#ifdef KERNEL
    typedef volatile uint32_t AtomicU32;
    typedef volatile uint64_t AtomicU64;
    typedef volatile bool     AtomicBool;
    #define CACHE_ALIGN __attribute__((aligned(64)))

    // volatile only stops the compiler from reordering; on arm64 the CPU can still make a
    // ring index visible before the data it publishes. Pair these with the user-space
    // std::atomic acquire loads and release stores.
    template <typename T> static inline T OzzyLoadAcquire(const volatile T* p) {
        return __atomic_load_n(p, __ATOMIC_ACQUIRE);
    }
    template <typename T> static inline void OzzyStoreRelease(volatile T* p, T v) {
        __atomic_store_n(p, v, __ATOMIC_RELEASE);
    }
    template <typename T> static inline void OzzyStoreRelaxed(volatile T* p, T v) {
        __atomic_store_n(p, v, __ATOMIC_RELAXED);
    }
#else
    #include <atomic>
    typedef std::atomic<uint32_t> AtomicU32;
    typedef std::atomic<uint64_t> AtomicU64;
    typedef std::atomic<bool>     AtomicBool;
    #define CACHE_ALIGN alignas(64)
#endif

// --- CONFIGURATION ---
#define kOzzySharedMemName          "/ozzy_shared_mem"
#define kOzzyMagic                  0x4F5A5A59 // "OZZY"
// OzzyKext user client: external method selectors and registry property
#define kOzzyMethodMidiOutPending   0          // OzzyMIDI queued bytes in midiOut
// OzzyHAL changes a device setting (common/ozzy_settings.h). Scalars in: setting id, value.
// The kext stops the transfers, re-runs the handshake with the setting and restarts them, then
// returns; the result is in audio.settings and audio.sampleRate. The HAL calls it from
// PerformDeviceConfigurationChange, with IO stopped.
#define kOzzyMethodApplySetting     1
#define kOzzyAudioPropertyKey       "OzzyAudio" // true if the device has audio channels

#define kOzzySharedVersion          6          // bump when OzzySharedMemory changes layout

// Sizing: 8KB covers High Speed USB max packet size, devices use what they need
#define kOzzyMaxPacketSize          8192
#define kOzzyNumPackets             128  // Total ring buffer size for HAL (large for stability)
#define kOzzyUSBBuffers             4    // Number of USB buffers in flight (ping-pong)
#define kOzzyPacketMask             (kOzzyUSBBuffers - 1)  // Mask for USB buffer rotation

// The kext publishes a timestamp anchor every kOzzyTimestampPackets packets; the HAL reports
// that spacing as the zero timestamp period. CoreAudio caps the IO buffer size at 3/8 of the
// period (measured: 8 packets of 80 frames -> 240 frames max, too small for 256+ buffers), so
// 40 packets (3200 frames) allows buffers up to 1200 frames. Must stay below kOzzyNumPackets.
#define kOzzyTimestampPackets       40

// OzzyAudioBuffer::deviceFlags
#define kOzzyDeviceFlagPloytecBulk  (1u << 0)  // Ploytec 512-byte "bulk" output layout, else 482-byte "interrupt"
#define kOzzyDeviceFlagPlainOutput  (1u << 1)  // outputBuffer is a ring of plain interleaved frames (see below)
#define kOzzyDeviceFlagSpdifMirror12 (1u << 2) // Ploytec bulk, 8 out: lanes 9/10 mirror pair 1/2 (ESU1808)

// Isochronous IN transfers (feedback): 2 in flight, up to 32 packets of up to 256 bytes each
#define kOzzyIsocInBufferSize       (2 * 32 * 256)

#define kOzzyMidiRingSize           1024 
#define kOzzyMidiRingMask           (kOzzyMidiRingSize - 1)

// --- GENERIC AUDIO BUFFER ---
struct OzzyAudioBuffer {
    AtomicBool hardwarePresent;
    AtomicBool driverReady;
    AtomicU32  sampleRate;
    
    // kOzzyDeviceFlagXxx
    AtomicU32  deviceFlags; 

    // Frame format info set by device engine
    uint32_t framesPerPacket;     // How many frames in each USB packet (e.g., 80 for Ploytec @ 96kHz)
    uint32_t samplesPerPacket;    // How many samples per USB packet (e.g., 10 for Ploytec)
    uint32_t outputBytesPerFrame; // Encoded bytes per frame for output (e.g., 48)
    uint32_t inputBytesPerFrame;  // Encoded bytes per frame for input (e.g., 64)
    uint32_t inputChannels;       // Audio channels the device sends (e.g., 8, or 16 for the WZ4)
    uint32_t outputChannels;      // Audio channels the device receives (e.g., 8, or 2 for the WZ4)
    uint32_t safetyOffsetFrames;  // How far ahead of the clock the engine reads output (0 = 2 packets)

    // Device settings (common/ozzy_settings.h) as the engine applied them at its last
    // handshake, and what the device reported. Written before driverReady at start, and before
    // kOzzyMethodApplySetting returns, which is when the HAL reads them.
    struct {
        uint32_t available;                   // bit (1 << enum ozzy_setting) per setting the device has
        uint8_t  value[OZZY_NUM_SETTINGS];
        uint8_t  clockLocked;                 // locked to S/PDIF (clock source Automatic only)
        uint32_t lockRate;                    // Hz while clockLocked
    } settings;

    struct {
        AtomicU32 sequence; 
        AtomicU64 sampleTime; 
        AtomicU64 hostTime;
    } timestamp;

    uint8_t _pad1[64]; 
    CACHE_ALIGN AtomicU64 halWritePosition;
    uint8_t _pad2[64];

    // The Engine writes raw data here. The HAL uses VID/PID to decide how to decode it.
    // Output: logical packets at kOzzyMaxPacketSize stride in the device's packet layout, or with
    // kOzzyDeviceFlagPlainOutput a ring of kOzzyNumPackets * framesPerPacket plain frames of
    // outputBytesPerFrame bytes (interleaved S24_3LE), which isochronous transfers read directly.
    uint8_t inputBuffer[kOzzyMaxPacketSize * kOzzyNumPackets];
    uint8_t outputBuffer[kOzzyMaxPacketSize * kOzzyNumPackets];
};

struct OzzyMidiBuffer {
    CACHE_ALIGN AtomicU32 writeIndex;
    uint8_t _pad1[64];
    CACHE_ALIGN AtomicU32 readIndex;
    uint8_t _pad2[64];
    uint8_t buffer[kOzzyMidiRingSize];
};

struct OzzySharedMemory {
    uint32_t magic;      // kOzzyMagic
    uint32_t version;
    uint32_t sessionID; 

#ifdef KERNEL
    AtomicU32 heartbeat;
#else
    AtomicU32 heartbeat { 0 };
#endif

    // The HAL reads these to load the correct plugin logic
    uint16_t vendorID;
    uint16_t productID;
    
    char manufacturerName[64];
    char productName[64];
    char serialNumber[64];
    
    OzzyMidiBuffer midiOut;
    OzzyMidiBuffer midiIn;
    
    // USB MIDI input buffer (for kext to receive MIDI from device)
    // Size must match endpoint's max packet size (512 bytes for bulk endpoints)
    uint8_t midiInUSBBuffer[512];

    // USB MIDI output buffer for devices with a MIDI OUT endpoint
    uint8_t midiOutUSBBuffer[64];

    // Isochronous IN transfers (feedback endpoint)
    uint8_t isocInUSBBuffer[kOzzyIsocInBufferSize];
    
    CACHE_ALIGN OzzyAudioBuffer audio;
};

#ifdef KERNEL
// --- MIDI rings, kext side: it fills midiIn and drains midiOut ---

// Append @count bytes to @ring; what doesn't fit is dropped. Returns how many went in.
static inline uint32_t OzzyMidiRingPush(volatile OzzyMidiBuffer* ring, const uint8_t* bytes, uint32_t count) {
    uint32_t w = ring->writeIndex;
    uint32_t r = OzzyLoadAcquire(&ring->readIndex);
    uint32_t i;
    for (i = 0; i < count && ((w + 1) & kOzzyMidiRingMask) != r; i++) {
        ring->buffer[w] = bytes[i];
        w = (w + 1) & kOzzyMidiRingMask;
    }
    OzzyStoreRelease(&ring->writeIndex, w);   // release: the bytes before the index
    return i;
}

// Take up to @max bytes from @ring into @dst. Returns how many.
static inline uint32_t OzzyMidiRingPop(volatile OzzyMidiBuffer* ring, uint8_t* dst, uint32_t max) {
    uint32_t r = ring->readIndex;
    uint32_t w = OzzyLoadAcquire(&ring->writeIndex);   // acquire: the bytes before the index
    uint32_t i;
    for (i = 0; i < max && r != w; i++) {
        dst[i] = ring->buffer[r];
        r = (r + 1) & kOzzyMidiRingMask;
    }
    OzzyStoreRelease(&ring->readIndex, r);
    return i;
}
#endif

#endif