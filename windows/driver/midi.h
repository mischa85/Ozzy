#ifndef WMIDI_H
#define WMIDI_H

/* The device's MIDI in and out as a MIDI circuit that Windows MIDI Services picks up: a "cyclic
 * UMP" kernel-streaming endpoint, the way Microsoft's USB MIDI 2 driver (also ACX) exposes its
 * ports. Apps on the old WinMM and WinRT MIDI APIs see MIDI 1.0 ports through the service.
 *
 * Each direction is a stream on its own pin whose ring of UMP messages is mapped into the
 * client (the MIDI service), with a read and a write position on a page of their own and an
 * event per side (KSPROPSETID_MidiLoopedStreaming). The engine's MIDI byte rings in the shared
 * memory are converted with common/midi_ump.c:
 *   - in (device -> host): wmidi_service, from the USB completion path, turns the engine's
 *     midi_in bytes into UMP messages in the client's ring
 *   - out (host -> device): a worker thread waits for the client's messages and queues their
 *     bytes for the engine's midi_out ring (ozzy_midi_queue_write)
 * Without a stream, MIDI in is dropped, as with ALSA rawmidi closed. */

#include "device.h"
#include "../../common/midi_ump.h"

/* One looped buffer shared with the client process */
struct wmidi_ring {
    uint8_t* buffer;              /* nonpaged; the kernel's view */
    ULONG size;                   /* page multiple */
    PMDL mdl[2];                  /* the same pages twice: mapped back to back for the client */
    PVOID user;                   /* the client's view, 2 * size long */
    volatile LONG* registers;     /* [0] read position, [1] write position (bytes) */
    PMDL registers_mdl;
    PVOID registers_user;
    PEPROCESS process;            /* the client (referenced), where the views live */
    PKEVENT write_event;          /* set by the side that wrote */
    PKEVENT read_event;           /* set by the side that read */
};

/* The context of a MIDI ACXSTREAM */
struct wmidi_stream {
    struct wdev* dev;
    bool in;                      /* device -> host */
    struct wmidi_ring ring;
    /* in: the byte to UMP state (completion path only) */
    struct midi_ump_encoder encoder;
    /* out: the worker and the bytes waiting for room in midi_out */
    PKTHREAD thread;
    KEVENT stop;
    struct ozzy_midi_queue queue;
    uint8_t queue_buf[4096];
};
typedef struct wmidi_stream wmidi_stream;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(wmidi_stream, wmidi_stream_get)

/* DeviceAdd: the MIDI circuit (added in PrepareHardware, like the audio ones) */
NTSTATUS wmidi_circuit_create(WDFDEVICE device, ACXCIRCUIT* circuit);

/* Any IRQL up to DISPATCH_LEVEL: the engine's MIDI in into the in stream's ring
 * (kmdf_bus.service, after every completion and timer tick) */
void wmidi_service(void* ctx);

#endif
