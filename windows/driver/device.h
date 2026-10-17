#ifndef WDEV_H
#define WDEV_H

#include <ntddk.h>
#include <wdf.h>
#include <acx.h>
#include <windef.h>
#include <ks.h>
#include <mmsystem.h>
#include <ksmedia.h>
#include "kmdf_bus.h"
#include "../../engine/shared/ozzy_shared_data.h"

#define OZZY_POOL_TAG 'yzzO'

/* One device: the engine and its bus on shared memory (as the kext has them), and an ACX
 * render and capture circuit for the model's channels */
struct wdev {
    const struct ozzy_device* model;   /* common/ozzy_devices.def row, from the hardware ID */
    uint32_t rate;                     /* the stream format's rate: the engine's default rate */
    struct kmdf_bus bus;
    struct ozzy_engine* engine;        /* ozzy_engine_size(model) bytes of nonpaged pool */
    struct ozzy_shared_memory* shm;
    ACXCIRCUIT render;                 /* NULL if the model has no channels that way */
    ACXCIRCUIT capture;
    bool circuits_added;
    bool configured;                   /* bus configured for the engine (once per PrepareHardware) */
};
typedef struct wdev wdev;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(wdev, wdev_get)

/* A circuit: its direction and its one format (ACX keeps a pointer to it) */
struct wcircuit {
    bool render;
    KSDATAFORMAT_WAVEFORMATEXTENSIBLE format;
};
typedef struct wcircuit wcircuit;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(wcircuit, wcircuit_get)

EVT_WDF_DRIVER_DEVICE_ADD wdev_evt_add;

/* circuit.c: the render (@render) or capture circuit with one S24_3LE format */
NTSTATUS wcircuit_create(WDFDEVICE device, bool render, ACXCIRCUIT* circuit);

#endif
