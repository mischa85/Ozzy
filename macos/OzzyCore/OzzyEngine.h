#ifndef OzzyEngine_h
#define OzzyEngine_h

#include "IOzzyBus.h"
#include "../../common/ozzy_ep.h"

// --- Hardware Description ---

// A pipe is a fixed endpoint address, or (address 0) the first endpoint on its interface
// of the given types and direction, the way the official Ploytec driver finds pipes. Same
// type as Linux uses (common/ozzy_ep.h). The kext reports the address it found through
// SetEndpoint().
typedef struct ozzy_ep OzzyPipeConfig;

enum OzzyPipeRole : uint8_t {
    kOzzyPipePcmOut,
    kOzzyPipePcmIn,
    kOzzyPipeMidiIn,
    kOzzyPipeMidiOut,
    kOzzyPipeFeedback,
    kOzzyPipeRoles
};

struct OzzyDeviceProfile {
    // Interfaces the driver must claim
    uint8_t interfaceCount;
    uint8_t interfaceIndices[4]; // Max 4 interfaces is plenty for audio
    uint8_t altSetting;          // Alternate setting selected on each claimed interface

    // Pipe Maps (0 = Unused)
    OzzyPipeConfig pcmOut;
    OzzyPipeConfig pcmIn;
    OzzyPipeConfig midiIn;
    OzzyPipeConfig midiOut;
    OzzyPipeConfig feedback;   // isochronous IN feedback for isochronous PCM out
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

    // Called by the kext for each pipe it opened, before Start(). transferType is
    // bmAttributes & 3 (1 = isochronous, 2 = bulk, 3 = interrupt); interval is bInterval.
    virtual void SetEndpoint(OzzyPipeRole role, uint8_t address, uint8_t transferType,
                             uint16_t maxPacketSize, uint8_t interval) {}

    // Lifecycle (Pure Virtual)
    virtual bool Start() = 0;
    virtual void Stop() = 0;

    // Callbacks
    virtual void OnPacketComplete(uint8_t pipeID, void* contextID, int status, uint32_t bytesTransferred) = 0;
    virtual void OnIsocComplete(uint8_t pipeID, void* contextID, int status, OzzyIsocPacket* packets, uint32_t count) {}

    // The MIDI driver queued bytes in the midiOut ring. Engines that send MIDI on their own
    // schedule (Ploytec embeds it in every PCM packet) ignore this.
    virtual void OnMidiOutPending() {}

    // The bus timer (IOzzyBus::StartTimer) fired
    virtual void OnTimer() {}
};

#endif