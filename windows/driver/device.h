#ifndef WDEV_H
#define WDEV_H

#include <ntifs.h>   /* ntddk.h and the process/virtual memory calls midi.c uses */
#include <wdf.h>
#include <acx.h>
#include <windef.h>
#include <ks.h>
#include <mmsystem.h>
#include <ksmedia.h>
#include "kmdf_bus.h"

struct wmidi_stream;
#include "../../engine/shared/ozzy_shared_data.h"

#define OZZY_POOL_TAG 'yzzO'

/* One device: the engine and its bus on shared memory (as the kext has them), and an ACX
 * render and capture circuit for the model's channels */
struct wdev {
    const struct ozzy_device* model;   /* common/ozzy_devices.def row, from the hardware ID */
    WDFWAITLOCK lock;                  /* engine restarts (rates, settings) and D0 transitions */
    bool started;                      /* the engine streams (D0) */
    struct wstream* streams[OZZY_PCM_DIRS];   /* each direction's stream, NULL if none */
    unsigned int holding[OZZY_PCM_DIRS];      /* running streams, which hold the rate (under lock) */
    struct kmdf_bus bus;
    struct ozzy_engine* engine;        /* ozzy_engine_size(model) bytes of nonpaged pool */
    struct ozzy_shared_memory* shm;
    ACXCIRCUIT render;                 /* NULL if the model has no channels that way */
    ACXCIRCUIT capture;
    ACXCIRCUIT midi;                   /* NULL if it couldn't be made (midi.c) */
    bool midi_added;
    struct wmidi_stream* volatile midi_streams[2];   /* the out and in stream, NULL if none */
    struct wmidi_stream* volatile midi_in;           /* the running in stream (wmidi_service) */
    volatile LONG midi_busy;           /* wmidi_service runs */
    bool circuits_added;
    bool configured;                   /* bus configured for the engine (once per PrepareHardware) */
    WCHAR name[128];                   /* the device's own name (its USB product string), or empty */
    WDFWORKITEM recover;               /* a wedged pipe: port reset and a new start (kbus dead) */
    WDFTIMER stats_timer;              /* a stats incident: the clients hear of it once it settles */
    volatile LONG stats_pending;
};
typedef struct wdev wdev;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(wdev, wdev_get)

/* A circuit: its direction and a format per rate the device lists (ACX keeps pointers to them) */
struct wcircuit {
    bool render;
    ACXEVENT changed;                  /* OZZY_KSEVENT_CHANGED (settings.c), NULL if none */
    KSDATAFORMAT_WAVEFORMATEXTENSIBLE formats[2 * OZZY_MAX_RATES];   /* direct out, then a speaker layout */
};
typedef struct wcircuit wcircuit;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(wcircuit, wcircuit_get)

EVT_WDF_DRIVER_DEVICE_ADD wdev_evt_add;

/* circuit.c: the render (@render) or capture circuit with an S24_3LE format per rate */
NTSTATUS wcircuit_create(WDFDEVICE device, bool render, ACXCIRCUIT* circuit);

/* PASSIVE_LEVEL: a stream's rate. Created (@apply false): whether the device can take it now;
 * run (@apply true): the engine at it (ozzy_engine_change_rate), a restart for a new rate, which
 * a running stream would not survive, so one of those refuses it; on success the stream holds
 * the rate (holding[], which it gives back when it pauses). Windows creates and prepares streams
 * of every format to try them; only a running one moves the device or holds its rate. */
NTSTATUS wdev_stream_rate(struct wdev* dev, bool render, uint32_t rate, bool apply);

/* settings.c: the control property set (windows/driver/ozzy_ksproperty.h) on a circuit, and the settings
 * the registry keeps, into the engine (PrepareHardware, after ozzy_engine_attach) */
NTSTATUS wsettings_assign(PACXCIRCUIT_INIT init);
void wsettings_restore(struct wdev* dev, WDFDEVICE device);

/* settings.c, PASSIVE_LEVEL: OZZY_KSEVENT_CHANGED on a created circuit, and its generation on
 * both audio circuits after a handshake or a setting change (without dev->lock held) */
NTSTATUS wsettings_add_event(ACXCIRCUIT circuit);
void wsettings_changed(struct wdev* dev);

#endif
