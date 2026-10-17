#ifndef KMDF_BUS_H
#define KMDF_BUS_H

/* The engine's bus (engine/core/ozzy_bus.h) for the Windows ACX driver: the engine's transfers
 * as KMDF USB requests on the shared memory, completed at DISPATCH_LEVEL. The engine encodes
 * PCM out and decodes PCM in right there (ozzy_engine_set_pcm_ring), and the ACX stream of
 * each direction hears about it (wstream_progress), so the real-time work happens in the
 * completion path. */

#include <ntddk.h>
#include <wdf.h>
#include <usb.h>
#include <wdfusb.h>
#include "../../engine/core/ozzy_engine.h"

struct wstream;

enum {
    KBUS_MAX_PIPES = 8,
    KBUS_REQUESTS_PER_PIPE = 8,   /* the engines keep at most 4 per pipe in flight */
};

struct kbus_slot {                /* one request's completion context */
    struct kmdf_bus* bus;
    uint8_t pipe;                 /* index in pipes */
    void* xfer;                   /* the engine's */
};

struct kbus_pipe {
    WDFUSBPIPE pipe;
    uint8_t address;
    WDFREQUEST requests[KBUS_REQUESTS_PER_PIPE];
    struct kbus_slot slots[KBUS_REQUESTS_PER_PIPE];
    volatile LONG busy;           /* bit per request in flight */
};

/* Lives in the device context (zeroed); kbus_init before use, kbus_release to start over */
struct kmdf_bus {
    struct ozzy_bus bus;          /* what the engine runs on (ozzy_engine_attach) */
    struct ozzy_engine* engine;
    WDFDEVICE device;
    WDFUSBDEVICE usb;
    PUSB_CONFIGURATION_DESCRIPTOR config;   /* for the raw endpoint descriptors */
    WDFMEMORY shm;
    uint8_t* shm_base;
    struct kbus_pipe pipes[KBUS_MAX_PIPES];
    int pipe_count;
    struct wstream* volatile stream[OZZY_PCM_DIRS];
    volatile LONG in_completion;
    PEX_TIMER timer;
};

void kbus_init(struct kmdf_bus* kb, struct ozzy_engine* engine);
/* ReleaseHardware: everything kbus_create and kbus_configure made, so PrepareHardware can run again */
void kbus_release(struct kmdf_bus* kb);

/* The USB target and its configuration (EvtDevicePrepareHardware) */
NTSTATUS kbus_create(struct kmdf_bus* kb, WDFDEVICE device);
/* Claim the profile's interfaces with its alternate setting, find each pipe, report it with
 * ozzy_engine_set_endpoint and create its requests on @shm (the transfers' memory) */
NTSTATUS kbus_configure(struct kmdf_bus* kb, void* shm, size_t shm_size);

/* Cancel every request in flight and wait for them (the engine has stopped resubmitting);
 * kbus_start_io makes the pipes usable again */
void kbus_stop_io(struct kmdf_bus* kb);
void kbus_start_io(struct kmdf_bus* kb);

/* The stream that hears about @dir's progress (NULL: none) */
void kbus_set_stream(struct kmdf_bus* kb, enum ozzy_pcm_dir dir, struct wstream* stream);
/* Wait until no completion is in progress: after detaching a ring or stream, before freeing it */
void kbus_wait_completions(struct kmdf_bus* kb);

void kbus_log(const char* fmt, ...);

#endif
