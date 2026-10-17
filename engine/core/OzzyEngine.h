#ifndef OzzyEngine_h
#define OzzyEngine_h

#include "IOzzyBus.h"
#include "../../common/ozzy_ep.h"
#include "../../common/ozzy_settings.h"
#include "../../common/ozzy_devices.h"
#include "../shared/OzzyPlatform.h"

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

// --- PCM rings ---
// An interleaved S24_3LE ring the engine encodes PCM out from, or decodes PCM in into, in its
// transfer completion path: where the OS's audio buffer can be reached from the kernel (the
// Windows driver's WaveRT buffer). Without one (macOS: the HAL writes the shared memory rings
// itself) the engine sends and receives the shared memory as it is.
enum OzzyPcmDir { kOzzyPcmOut, kOzzyPcmIn, kOzzyPcmDirs };

struct OzzyPcmRing {
    uint8_t* base;
    uint32_t frames;     // ring length
    uint32_t channels;   // must be the model's channel count in this direction
    uint64_t start;      // ring position of the next packet's first frame (frame start % frames)
};

// --- Base Class ---
class OzzyEngine {
protected:
    IOzzyBus* mBus = nullptr;
    void* mSharedMemory = nullptr; // Opaque pointer to OzzySharedMemory
    struct ozzy_settings mSettings = {};  // what Start() sends (Init: the defaults)
    const OzzyPcmRing* volatile mPcmRingSet[kOzzyPcmDirs] = {};   // SetPcmRing

    // Publish mSettings and the clock status of the handshake Start() just ran
    void PublishSettings(const struct ozzy_clock_status& clock);

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

    // --- Device settings (common/ozzy_settings.h) ---
    // The model's row in common/ozzy_devices.def: its quirks and channels say which settings
    // it has
    virtual const struct ozzy_device* GetModel() const { return nullptr; }
    // The model's quirks as far as settings go (an engine drops what its transport can't do)
    virtual uint32_t GetSettingQuirks() const { return GetModel() ? GetModel()->quirks : 0; }
    bool HasSetting(unsigned int id) const {
        const struct ozzy_device* m = GetModel();
        return m && ozzy_setting_values(GetSettingQuirks(), m->in_channels, m->out_channels, id) > 0;
    }
    void SetSetting(unsigned int id, uint8_t value) { if (id < OZZY_NUM_SETTINGS) mSettings.value[id] = value; }

    // Attach @ring for @dir (nullptr detaches). The completion path picks it up with the next
    // packet. The ring must stay valid until the caller has waited out the completions in
    // progress after detaching.
    void SetPcmRing(OzzyPcmDir dir, const OzzyPcmRing* ring) { OzzyStoreRelease(&mPcmRingSet[dir], ring); }

    // Restart for changed settings: Suspend() stops resubmitting transfers and the timer, the
    // kext aborts every pipe, then calls Start() again. Unlike Stop() it keeps hardwarePresent
    // and driverReady, so the HAL keeps the device published (CoreAudio has stopped IO).
    virtual void Suspend() {}
};

#endif