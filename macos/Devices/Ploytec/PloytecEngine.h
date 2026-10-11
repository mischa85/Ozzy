#ifndef PloytecEngine_h
#define PloytecEngine_h

#include "../../OzzyCore/OzzyEngine.h"
#include "../../Shared/OzzySharedData.h"
#include "PloytecProtocol.h"
#include "../../../common/devices/ploytec/ploytec_models.h"

class PloytecEngine : public OzzyEngine {
    uint64_t mHwSampleTime = 0;
    enum ploytec_out_layout mOutLayout = PLOYTEC_OUT_LAYOUT_INT;
    uint32_t mSampleRate = 96000;
    const struct ploytec_model* mModel = nullptr;
    uint32_t mPacketSizeOut = 0;
    
    // Storage for the profile
    OzzyDeviceProfile mProfile;
    
    // Helper to cast the generic pointer
    volatile OzzySharedMemory* GetSHM() { return (volatile OzzySharedMemory*)mSharedMemory; }

public:
    PloytecEngine(uint16_t pid);
    
    virtual const OzzyDeviceProfile& GetProfile() const override { return mProfile; }
    virtual void SetOutEndpoint(bool isBulk, uint16_t maxPacketSize) override;
    virtual bool Start() override;
    virtual void Stop() override;
    virtual void OnPacketComplete(uint8_t pipeID, void* ctx, int status, uint32_t bytes) override;
    
private:
    void SubmitPCMOut(uint32_t packetIdx);
    void SubmitPCMIn(uint32_t packetIdx);
    void SubmitMIDIIn(uint32_t idx);
    void UpdateTimestamp();
    void ProcessMIDIOutput(uint32_t packetIdx);
    void ProcessMIDIInput(uint32_t bytesReceived);
};

#endif