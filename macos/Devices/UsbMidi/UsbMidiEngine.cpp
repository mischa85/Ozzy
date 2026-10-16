#include "UsbMidiEngine.h"
#include "../../Shared/OzzyLog.h"
#include <string.h>

// Xone:1D (lsusb in #27): interface 1 is MIDIStreaming with one embedded jack per direction
#define kMidiInterface  1
#define kEpMidiIn       0x81
#define kEpMidiOut      0x01
#define kCable          0

// Read one event packet per transfer. The endpoint's wMaxPacketSize is 4, so a longer
// transfer would only complete once it filled up, holding back every event until then.
#define kReadSize       USB_MIDI_PACKET_SIZE

// A read slot stops after this many errors in a row instead of resubmitting forever
#define kMaxReadErrors  8

UsbMidiEngine::UsbMidiEngine() {
    mProfile.interfaceCount = 1;
    mProfile.interfaceIndices[0] = kMidiInterface;
    mProfile.altSetting = 0;
    mProfile.midiIn.address = kEpMidiIn;
    mProfile.midiIn.intf = kMidiInterface;
    mProfile.midiOut.address = kEpMidiOut;
    mProfile.midiOut.intf = kMidiInterface;
}

bool UsbMidiEngine::Start() {
    auto* shm = GetSHM();
    if (!shm) return false;

    // The device calls itself "Xone MIDI Device"; use the name on the front panel
    strlcpy((char*)shm->productName, "Xone:1D", sizeof(shm->productName));

    // No audio: the HAL skips devices without channels
    shm->audio.inputChannels = 0;
    shm->audio.outputChannels = 0;

    memset(&mEncoder, 0, sizeof(mEncoder));
    __atomic_store_n(&mOutBusy, false, __ATOMIC_RELEASE);
    mRunning = true;
    OzzyStoreRelease(&shm->audio.hardwarePresent, true);

    for (uint32_t slot = 0; slot < kNumReads; slot++) {
        mReadErrors[slot] = 0;
        SubmitRead(slot);
    }

    OzzyStoreRelease(&shm->audio.driverReady, true);
    LogUsbMidi("Xone:1D ready");
    return true;
}

void UsbMidiEngine::Stop() {
    mRunning = false;
    auto* shm = GetSHM();
    if (!shm) return;
    shm->audio.driverReady = false;
    shm->audio.hardwarePresent = false;
    LogUsbMidi("engine stopped");
}

void UsbMidiEngine::SubmitRead(uint32_t slot) {
    auto* shm = GetSHM();
    void* buf = (void*)(shm->midiInUSBBuffer + slot * kReadSize);
    if (!mBus->SubmitUSBPacket(kEpMidiIn, buf, kReadSize, (void*)(uintptr_t)slot)) {
        LogUsbMidi("MIDI input submission failed (slot %u)", slot);
    }
}

/* Unpack the event packets into the midiIn ring that OzzyMIDI reads */
void UsbMidiEngine::ProcessRead(uint32_t slot, uint32_t bytes) {
    auto* shm = GetSHM();
    const uint8_t* packets = (const uint8_t*)(shm->midiInUSBBuffer + slot * kReadSize);

    for (uint32_t offset = 0; offset + USB_MIDI_PACKET_SIZE <= bytes; offset += USB_MIDI_PACKET_SIZE) {
        uint8_t midi[3];
        unsigned int len = usb_midi_decode_packet(packets + offset, midi);
        for (unsigned int i = 0; i < len; i++) {
            uint32_t w = shm->midiIn.writeIndex;
            uint32_t nextW = (w + 1) & kOzzyMidiRingMask;
            if (nextW == OzzyLoadAcquire(&shm->midiIn.readIndex)) return;  // ring full: drop the rest
            shm->midiIn.buffer[w] = midi[i];
            OzzyStoreRelease(&shm->midiIn.writeIndex, nextW);
        }
    }
}

void UsbMidiEngine::OnPacketComplete(uint8_t pipeID, void* ctx, int status, uint32_t bytes) {
    if (!mBus || !mRunning) return;

    if (pipeID == kEpMidiIn) {
        uint32_t slot = (uint32_t)(uintptr_t)ctx;
        if (status != 0) {
            if (++mReadErrors[slot] >= kMaxReadErrors) {
                LogUsbMidi("MIDI input slot %u stopped after %u errors (last 0x%x)", slot, mReadErrors[slot], status);
                return;
            }
        } else {
            mReadErrors[slot] = 0;
            ProcessRead(slot, bytes);
        }
        SubmitRead(slot);
    } else if (pipeID == kEpMidiOut) {
        if (status != 0) LogUsbMidi("MIDI output error 0x%x", status);
        __atomic_store_n(&mOutBusy, false, __ATOMIC_RELEASE);
        PumpMidiOut();
    }
}

void UsbMidiEngine::OnMidiOutPending() {
    if (mRunning) PumpMidiOut();
}

/* Send the next event packet from the midiOut ring. One OUT transfer is in flight at a time;
 * mOutBusy hands the sender role between the MIDI driver's kick and the OUT completion. */
void UsbMidiEngine::PumpMidiOut() {
    auto* shm = GetSHM();
    if (!shm) return;

    for (;;) {
        if (__atomic_exchange_n(&mOutBusy, true, __ATOMIC_ACQUIRE)) return;

        uint32_t r = shm->midiOut.readIndex;
        uint32_t w = OzzyLoadAcquire(&shm->midiOut.writeIndex);
        uint8_t* packet = (uint8_t*)shm->midiOutUSBBuffer;

        while (r != w) {
            uint8_t byte = shm->midiOut.buffer[r];
            r = (r + 1) & kOzzyMidiRingMask;
            if (usb_midi_encode_byte(&mEncoder, kCable, byte, packet)) {
                OzzyStoreRelease(&shm->midiOut.readIndex, r);
                if (mBus->SubmitUSBPacket(kEpMidiOut, packet, USB_MIDI_PACKET_SIZE, nullptr)) return;
                LogUsbMidi("MIDI output submission failed");
                break;
            }
        }
        OzzyStoreRelease(&shm->midiOut.readIndex, r);
        __atomic_store_n(&mOutBusy, false, __ATOMIC_RELEASE);

        // A kick that came in while we held mOutBusy returned early; pick up its bytes
        if (OzzyLoadAcquire(&shm->midiOut.writeIndex) == r) return;
    }
}
