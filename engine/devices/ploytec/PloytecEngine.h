#ifndef PloytecEngine_h
#define PloytecEngine_h

#include "../../core/OzzyEngine.h"
#include "../../shared/OzzySharedData.h"
#include "PloytecProtocol.h"
#include "../../../common/ozzy_devices.h"
#include "../../../common/devices/ploytec/ploytec_midi.h"

struct ploytec_host_ops;
extern const struct ploytec_host_ops kPloytecHostOps;   // handshake host services over IOzzyBus

class PloytecEngine : public OzzyEngine {
    uint64_t mHwSampleTime = 0;
    enum ploytec_out_layout mOutLayout = PLOYTEC_OUT_LAYOUT_INT;
    uint32_t mSampleRate = 96000;   // what the device runs at (the S/PDIF lock rate while locked)
    uint32_t mDefaultRate = 96000;  // what Start() asks for: the highest rate
    volatile bool mRunning = false; // completions resubmit (cleared by Suspend/Stop)
    const struct ozzy_device* mModel = nullptr;   // row in common/ozzy_devices.def
    uint32_t mPacketSizeOut = 0;
    bool mPcmInInt1024 = false;     // PCM IN is interrupt, wMaxPacketSize 1024 (ploytec_midi.h)
    struct ploytec_midi mMidi = {}; // MIDI framing at mSampleRate
    
    // Storage for the profile
    OzzyDeviceProfile mProfile;
    
    // Helper to cast the generic pointer
    volatile OzzySharedMemory* GetSHM() { return (volatile OzzySharedMemory*)mSharedMemory; }

public:
    explicit PloytecEngine(const struct ozzy_device* device);
    
    virtual const OzzyDeviceProfile& GetProfile() const override { return mProfile; }
    virtual void SetEndpoint(OzzyPipeRole role, uint8_t address, uint8_t transferType,
                             uint16_t maxPacketSize, uint8_t interval) override;
    virtual bool Start() override;
    virtual void Stop() override;
    virtual void OnPacketComplete(uint8_t pipeID, void* ctx, int status, uint32_t bytes) override;
    virtual const struct ozzy_device* GetModel() const override { return mModel; }
    // The ESU1808's mirror only on the bulk layout (official dmaEncode08_Int964 on interrupt)
    virtual uint32_t GetSettingQuirks() const override {
        return mOutLayout == PLOYTEC_OUT_LAYOUT_BULK ? mModel->quirks : (mModel->quirks & ~(uint32_t)OZZY_QUIRK_SPDIF_MIRROR);
    }
    virtual void Suspend() override;
    
private:
    void SubmitPCMOut(uint32_t packetIdx);
    void SubmitPCMIn(uint32_t packetIdx);
    void SubmitMIDIIn(uint32_t idx);
    void UpdateTimestamp();
    void ProcessMIDIOutput(uint32_t packetIdx);
    void ProcessMIDIInput(uint32_t bytesReceived);
};

#endif