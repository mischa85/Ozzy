#ifndef UsbMidiEngine_h
#define UsbMidiEngine_h

#include "../../OzzyCore/OzzyEngine.h"
#include "../../Shared/OzzySharedData.h"
#include "../../../common/usb_midi.h"

/*
 * MIDI-only devices that speak the USB MIDI class protocol but need Ozzy because macOS's
 * class driver won't take them. The Xone:1D's bulk endpoints have wMaxPacketSize 4, which
 * USB doesn't allow for full speed bulk; Linux's snd-usb-audio accepts it, macOS doesn't.
 * MIDI goes through the same shared memory rings as the Ploytec devices, so OzzyMIDI
 * handles both. No audio: the engine publishes 0 channels and the HAL skips the device.
 */
class UsbMidiEngine : public OzzyEngine {
    static constexpr uint32_t kNumReads = 2;

    OzzyDeviceProfile mProfile;
    struct usb_midi_encoder mEncoder;   // only touched while mOutBusy is held
    bool mRunning = false;
    bool mOutBusy = false;
    uint32_t mReadErrors[kNumReads] = {};

    volatile OzzySharedMemory* GetSHM() { return (volatile OzzySharedMemory*)mSharedMemory; }

public:
    UsbMidiEngine();

    virtual const OzzyDeviceProfile& GetProfile() const override { return mProfile; }
    virtual bool Start() override;
    virtual void Stop() override;
    virtual void OnPacketComplete(uint8_t pipeID, void* ctx, int status, uint32_t bytes) override;
    virtual void OnMidiOutPending() override;

private:
    void SubmitRead(uint32_t slot);
    void ProcessRead(uint32_t slot, uint32_t bytes);
    void PumpMidiOut();
};

#endif
