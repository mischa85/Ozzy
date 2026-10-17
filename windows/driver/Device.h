#ifndef OzzyDevice_h
#define OzzyDevice_h

#include <ntddk.h>
#include <wdf.h>
#include <acx.h>
#include <windef.h>
#include <ks.h>
#include <mmsystem.h>
#include <ksmedia.h>
#include "KmdfBus.h"
#include "../../engine/shared/ozzy_shared_data.h"

#define OZZY_POOL_TAG 'yzzO'

// One device: the engine and its bus on shared memory (as the kext has them), and an ACX
// render and capture circuit for the model's channels
struct OzzyDeviceContext {
    const struct ozzy_device* model;   // common/ozzy_devices.def row, from the hardware ID
    uint32_t rate;                     // the stream format's rate: the engine's default rate
    KmdfBus* bus;
    struct ozzy_engine* engine;        // ozzy_engine_size(model) bytes of nonpaged pool
    ozzy_shared_memory* shm;
    ACXCIRCUIT render;                 // nullptr if the model has no channels that way
    ACXCIRCUIT capture;
    bool circuitsAdded;
    bool configured;                   // bus configured for the engine (once per PrepareHardware)
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(OzzyDeviceContext, GetOzzyDevice)

// A circuit: its direction and its one format (ACX keeps a pointer to it)
struct OzzyCircuitContext {
    bool render;
    KSDATAFORMAT_WAVEFORMATEXTENSIBLE format;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(OzzyCircuitContext, GetOzzyCircuit)

EVT_WDF_DRIVER_DEVICE_ADD OzzyEvtDeviceAdd;

// Circuit.cpp: the render (@render) or capture circuit with one S24_3LE format
NTSTATUS OzzyCreateCircuit(WDFDEVICE device, bool render, ACXCIRCUIT* circuit);

#endif
