#ifndef OzzyEngine_h
#define OzzyEngine_h

#include "IOzzyBus.h"

// --- Hardware Description ---
struct OzzyPipeConfig {
    uint8_t address;        // e.g., 0x05
    uint8_t interfaceIndex; // Which interface owns this pipe? (0, 1, etc.)
};

struct OzzyDeviceProfile {
    // Interfaces the driver must claim
    uint8_t interfaceCount;
    uint8_t interfaceIndices[4]; // Max 4 interfaces is plenty for audio

    // Pipe Maps (0 = Unused)
    OzzyPipeConfig pcmOut;
    OzzyPipeConfig pcmIn;
    OzzyPipeConfig midiIn;
};

// --- Base Class ---
class OzzyEngine {
protected:
    IOzzyBus* mBus = nullptr;
    void* mSharedMemory = nullptr; // Opaque pointer to OzzySharedMemory

public:
    virtual ~OzzyEngine() {}

    // Initialization (Implemented in .cpp)
    virtual void Init(IOzzyBus* bus, void* sharedMemory);

    // New: The Kext calls this to ask "What hardware do you need?"
    virtual const OzzyDeviceProfile& GetProfile() const = 0;

    // Called by kext after probing the PCM output endpoint descriptor, before Start()
    virtual void SetOutEndpoint(bool isBulk, uint16_t maxPacketSize) = 0;

    // Lifecycle (Pure Virtual)
    virtual bool Start() = 0;
    virtual void Stop() = 0;

    // Callbacks
    virtual void OnPacketComplete(uint8_t pipeID, void* contextID, int status, uint32_t bytesTransferred) = 0;
};

#endif