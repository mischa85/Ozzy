#ifndef PloytecIsocEngine_h
#define PloytecIsocEngine_h

#include "../../OzzyCore/OzzyEngine.h"
#include "../../OzzyCore/OzzyIsocOutput.h"
#include "../../Shared/OzzySharedData.h"
#include "PloytecProtocol.h"
#include "../../../common/devices/ploytec/ploytec_models.h"
#include "../../../common/devices/ploytec/ploytec_midi.h"

/*
 * Ploytec bulk in / isochronous out devices (Akai EIE Pro, .claude/re/akai_eie_pro.md).
 * The handshake, PCM in and MIDI in are the Xone ones (PloytecIO.h). PCM out is plain
 * S24_3LE, run by OzzyIsocOutput from the device's feedback. MIDI out goes to its own
 * endpoint in 9-byte transfers, one per millisecond from the bus timer, like Linux.
 * Endpoints are found by type, as described in ploytec_defs.h for both platforms.
 */
class PloytecIsocEngine : public OzzyEngine {
    static constexpr uint32_t kInTransfers = 2;
    static constexpr uint32_t kMidiInSlot = 256;   // bytes per MIDI IN read slot in midiInUSBBuffer

    OzzyDeviceProfile mProfile;
    const struct ploytec_model* mModel = nullptr;
    uint32_t mSampleRate = 0;
    struct ploytec_midi mMidi = {};
    volatile bool mRunning = false;

    // Endpoints as the kext found them
    uint8_t mOutAddr = 0, mInAddr = 0, mFbAddr = 0, mMidiInAddr = 0, mMidiOutAddr = 0;
    uint16_t mOutMaxPacket = 0, mFbMaxPacket = 0;
    uint8_t mOutInterval = 1, mFbInterval = 0;

    OzzyIsocOutput mOutput;
    bool mMidiOutBusy = false;       // one MIDI OUT transfer in flight

    volatile OzzySharedMemory* GetSHM() { return (volatile OzzySharedMemory*)mSharedMemory; }

public:
    PloytecIsocEngine(uint16_t vid, uint16_t pid);

    virtual const OzzyDeviceProfile& GetProfile() const override { return mProfile; }
    virtual void SetEndpoint(OzzyPipeRole role, uint8_t address, uint8_t transferType,
                             uint16_t maxPacketSize, uint8_t interval) override;
    virtual bool Start() override;
    virtual void Stop() override;
    virtual void OnPacketComplete(uint8_t pipeID, void* ctx, int status, uint32_t bytes) override;
    virtual void OnIsocComplete(uint8_t pipeID, void* ctx, int status, OzzyIsocPacket* packets, uint32_t count) override;
    virtual void OnTimer() override;

private:
    void SubmitMIDIIn(uint32_t slot);
    void SendMidiOut();
};

#endif
