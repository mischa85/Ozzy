#ifndef OzzySharedData_h
#define OzzySharedData_h

#include <stdint.h>

// --- KERNEL vs USER SPACE SHIM ---
#ifdef KERNEL
    typedef volatile uint32_t AtomicU32;
    typedef volatile uint64_t AtomicU64;
    typedef volatile bool     AtomicBool;
    #define CACHE_ALIGN __attribute__((aligned(64)))
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

// Sizing: 8KB covers High Speed USB max packet size, devices use what they need
#define kOzzyMaxPacketSize          8192
#define kOzzyNumPackets             128  // Total ring buffer size for HAL (large for stability)
#define kOzzyUSBBuffers             4    // Number of USB buffers in flight (ping-pong)
#define kOzzyPacketMask             (kOzzyUSBBuffers - 1)  // Mask for USB buffer rotation

#define kOzzyMidiRingSize           1024 
#define kOzzyMidiRingMask           (kOzzyMidiRingSize - 1)

// --- GENERIC AUDIO BUFFER ---
struct OzzyAudioBuffer {
    AtomicBool hardwarePresent;
    AtomicBool driverReady;
    AtomicU32  sampleRate;
    
    // Generic flags (Bit 0: Bulk Mode for Ploytec, etc.)
    AtomicU32  deviceFlags; 

    // Frame format info set by device engine
    uint32_t framesPerPacket;     // How many frames in each USB packet (e.g., 80 for Ploytec @ 96kHz)
    uint32_t samplesPerPacket;    // How many samples per USB packet (e.g., 10 for Ploytec)
    uint32_t outputBytesPerFrame; // Encoded bytes per frame for output (e.g., 48)
    uint32_t inputBytesPerFrame;  // Encoded bytes per frame for input (e.g., 64)

    struct {
        AtomicU32 sequence; 
        AtomicU64 sampleTime; 
        AtomicU64 hostTime;
    } timestamp;

    uint8_t _pad1[64]; 
    CACHE_ALIGN AtomicU64 halWritePosition;
    uint8_t _pad2[64];

    // The Engine writes raw data here. The HAL uses VID/PID to decide how to decode it.
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
    
    CACHE_ALIGN OzzyAudioBuffer audio;
};

#endif