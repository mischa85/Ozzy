#include "UsbMidiEngine.h"
#include "../../shared/ozzy_log.h"
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
    ozzy_strlcpy((char*)shm->product_name, "Xone:1D", sizeof(shm->product_name));

    // No audio: the HAL skips devices without channels
    shm->audio.input_channels = 0;
    shm->audio.output_channels = 0;

    memset(&mEncoder, 0, sizeof(mEncoder));
    ozzy_store_release_bool(&mOutBusy, false);
    mRunning = true;
    ozzy_store_release_bool(&shm->audio.hardware_present, true);

    for (uint32_t slot = 0; slot < kNumReads; slot++) {
        mReadErrors[slot] = 0;
        SubmitRead(slot);
    }

    ozzy_store_release_bool(&shm->audio.driver_ready, true);
    usb_midi_log("Xone:1D ready");
    return true;
}

void UsbMidiEngine::Stop() {
    mRunning = false;
    auto* shm = GetSHM();
    if (!shm) return;
    shm->audio.driver_ready = false;
    shm->audio.hardware_present = false;
    usb_midi_log("engine stopped");
}

void UsbMidiEngine::SubmitRead(uint32_t slot) {
    auto* shm = GetSHM();
    void* buf = (void*)(shm->midi_in_usb_buffer + slot * kReadSize);
    if (!mBus->SubmitUSBPacket(kEpMidiIn, buf, kReadSize, (void*)(uintptr_t)slot)) {
        usb_midi_log("MIDI input submission failed (slot %u)", slot);
    }
}

/* Unpack the event packets into the midiIn ring that OzzyMIDI reads */
void UsbMidiEngine::ProcessRead(uint32_t slot, uint32_t bytes) {
    auto* shm = GetSHM();
    const uint8_t* packets = (const uint8_t*)(shm->midi_in_usb_buffer + slot * kReadSize);

    for (uint32_t offset = 0; offset + USB_MIDI_PACKET_SIZE <= bytes; offset += USB_MIDI_PACKET_SIZE) {
        uint8_t midi[3];
        unsigned int len = usb_midi_decode_packet(packets + offset, midi);
        if (ozzy_midi_ring_push(&shm->midi_in, midi, len) < len) return;  // ring full: drop the rest
    }
}

void UsbMidiEngine::OnPacketComplete(uint8_t pipeID, void* ctx, int status, uint32_t bytes) {
    if (!mBus || !mRunning) return;

    if (pipeID == kEpMidiIn) {
        uint32_t slot = (uint32_t)(uintptr_t)ctx;
        if (status != 0) {
            if (++mReadErrors[slot] >= kMaxReadErrors) {
                usb_midi_log("MIDI input slot %u stopped after %u errors (last 0x%x)", slot, mReadErrors[slot], status);
                return;
            }
        } else {
            mReadErrors[slot] = 0;
            ProcessRead(slot, bytes);
        }
        SubmitRead(slot);
    } else if (pipeID == kEpMidiOut) {
        if (status != 0) usb_midi_log("MIDI output error 0x%x", status);
        ozzy_store_release_bool(&mOutBusy, false);
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
        if (ozzy_xchg_acquire_bool(&mOutBusy, true)) return;

        uint32_t r = shm->midi_out.read_index;
        uint32_t w = ozzy_load_acquire_u32(&shm->midi_out.write_index);
        uint8_t* packet = (uint8_t*)shm->midi_out_usb_buffer;

        while (r != w) {
            uint8_t byte = shm->midi_out.buffer[r];
            r = (r + 1) & OZZY_MIDI_RING_MASK;
            if (usb_midi_encode_byte(&mEncoder, kCable, byte, packet)) {
                ozzy_store_release_u32(&shm->midi_out.read_index, r);
                if (mBus->SubmitUSBPacket(kEpMidiOut, packet, USB_MIDI_PACKET_SIZE, nullptr)) return;
                usb_midi_log("MIDI output submission failed");
                break;
            }
        }
        ozzy_store_release_u32(&shm->midi_out.read_index, r);
        ozzy_store_release_bool(&mOutBusy, false);

        // A kick that came in while we held mOutBusy returned early; pick up its bytes
        if (ozzy_load_acquire_u32(&shm->midi_out.write_index) == r) return;
    }
}
