#include "device.h"
#include "ozzy_ksproperty.h"

/* ozzyctl's property set (ozzy_ksproperty.h) on each circuit's filter: the settings as the engine
 * has them, and a change, which the engine checks and applies (ozzy_engine_change_setting).
 * KS property requests arrive at PASSIVE_LEVEL. Each change is kept in the device's registry
 * key and applied again when the device comes up (wsettings_restore), the way alsactl and
 * CoreAudio's plug-in storage do it on the other platforms. */

static const GUID ozzy_propsetid_settings = OZZY_PROPSETID_SETTINGS_INIT;

/* The registry value of setting @id: its name (ozzy_setting_descs), as UTF-16 in @buf */
static void wsettings_value_name(unsigned int id, WCHAR* buf, USHORT chars, UNICODE_STRING* name) {
    const char* s = ozzy_setting_descs[id].name;
    USHORT n = 0;
    while (s[n] && n + 1 < chars) {
        buf[n] = (WCHAR)(unsigned char)s[n];
        n++;
    }
    buf[n] = 0;
    name->Buffer = buf;
    name->Length = (USHORT)(n * sizeof(WCHAR));
    name->MaximumLength = (USHORT)(chars * sizeof(WCHAR));
}

static void wsettings_save(WDFDEVICE device, unsigned int id, uint32_t value) {
    WDFKEY key;
    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(device, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE, WDF_NO_OBJECT_ATTRIBUTES,
                                             &key)))
        return;
    WCHAR buf[64];
    UNICODE_STRING name;
    wsettings_value_name(id, buf, 64, &name);
    WdfRegistryAssignULong(key, &name, value);
    WdfRegistryClose(key);
}

void wsettings_restore(struct wdev* dev, WDFDEVICE device) {
    PAGED_CODE();
    WDFKEY key;
    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(device, PLUGPLAY_REGKEY_DEVICE, KEY_READ, WDF_NO_OBJECT_ATTRIBUTES,
                                             &key)))
        return;
    for (unsigned int id = 0; id < OZZY_NUM_SETTINGS; id++) {
        WCHAR buf[64];
        UNICODE_STRING name;
        ULONG value;
        wsettings_value_name(id, buf, 64, &name);
        if (NT_SUCCESS(WdfRegistryQueryULong(key, &name, &value)) && value < ozzy_engine_setting_values(dev->engine, id))
            ozzy_engine_set_setting(dev->engine, id, (uint8_t)value);
    }
    WdfRegistryClose(key);
}

static void wsettings_status(const struct wdev* dev, struct ozzy_ksproperty_status* st) {
    RtlZeroMemory(st, sizeof(*st));
    st->version = OZZY_KSPROPERTY_VERSION;
    st->in_channels = dev->model->in_channels;
    st->out_channels = dev->model->out_channels;
    if (!dev->engine) return;
    for (unsigned int id = 0; id < OZZY_NUM_SETTINGS; id++) {
        if (ozzy_engine_has_setting(dev->engine, id)) st->available |= 1u << id;
        st->value[id] = dev->engine->settings.value[id];
    }
    if (dev->shm) {
        st->sample_rate = dev->shm->audio.sample_rate;
        st->clock_locked = dev->shm->audio.settings.clock_locked;
        st->lock_rate = dev->shm->audio.settings.lock_rate;
    }
}

/* OZZY_KSPROPERTY_STATUS (get) */
static VOID wsettings_evt_status(WDFOBJECT object, WDFREQUEST request) {
    PAGED_CODE();
    WDFDEVICE device = AcxCircuitGetWdfDevice((ACXCIRCUIT)object);
    struct wdev* dev = wdev_get(device);
    ACX_REQUEST_PARAMETERS params;
    ACX_REQUEST_PARAMETERS_INIT(&params);
    AcxRequestGetParameters(request, &params);

    NTSTATUS status;
    ULONG_PTR size = sizeof(struct ozzy_ksproperty_status);
    if (params.Parameters.Property.ValueCb == 0) {
        status = STATUS_BUFFER_OVERFLOW;   /* the caller asks for the size */
    } else if (params.Parameters.Property.ValueCb < size || !params.Parameters.Property.Value) {
        status = STATUS_BUFFER_TOO_SMALL;
        size = 0;
    } else {
        WdfWaitLockAcquire(dev->lock, NULL);
        wsettings_status(dev, (struct ozzy_ksproperty_status*)params.Parameters.Property.Value);
        WdfWaitLockRelease(dev->lock);
        status = STATUS_SUCCESS;
    }
    WdfRequestCompleteWithInformation(request, status, size);
}

/* OZZY_KSPROPERTY_SETTING (set): checked by the engine; a device setting restarts it */
static VOID wsettings_evt_setting(WDFOBJECT object, WDFREQUEST request) {
    PAGED_CODE();
    WDFDEVICE device = AcxCircuitGetWdfDevice((ACXCIRCUIT)object);
    struct wdev* dev = wdev_get(device);
    ACX_REQUEST_PARAMETERS params;
    ACX_REQUEST_PARAMETERS_INIT(&params);
    AcxRequestGetParameters(request, &params);

    const struct ozzy_ksproperty_setting* set = (const struct ozzy_ksproperty_setting*)params.Parameters.Property.Value;
    if (params.Parameters.Property.ValueCb < sizeof(*set) || !set) {
        WdfRequestComplete(request, STATUS_BUFFER_TOO_SMALL);
        return;
    }

    NTSTATUS status = STATUS_SUCCESS;
    WdfWaitLockAcquire(dev->lock, NULL);
    enum ozzy_setting_change change =
        dev->engine ? ozzy_engine_change_setting(dev->engine, set->id, set->value) : OZZY_SETTING_INVALID;
    if (change == OZZY_SETTING_INVALID) {
        status = STATUS_INVALID_PARAMETER;
    } else if (change == OZZY_SETTING_RESTART && dev->started) {
        dev->started = ozzy_engine_restart(dev->engine);
        if (!dev->started) status = STATUS_DEVICE_NOT_READY;
    }
    WdfWaitLockRelease(dev->lock);

    /* Kept even if the device didn't take it now: the next start sends it again */
    if (change != OZZY_SETTING_INVALID && change != OZZY_SETTING_SAME) wsettings_save(device, set->id, set->value);
    WdfRequestComplete(request, status);
}

NTSTATUS wsettings_assign(PACXCIRCUIT_INIT init) {
    static ACX_PROPERTY_ITEM properties[] = {
        {
            .Set = &ozzy_propsetid_settings,
            .Id = OZZY_KSPROPERTY_STATUS,
            .Flags = ACX_PROPERTY_ITEM_FLAG_GET,
            .EvtAcxObjectProcessRequest = wsettings_evt_status,
            .ValueCb = sizeof(struct ozzy_ksproperty_status),
        },
        {
            .Set = &ozzy_propsetid_settings,
            .Id = OZZY_KSPROPERTY_SETTING,
            .Flags = ACX_PROPERTY_ITEM_FLAG_SET,
            .EvtAcxObjectProcessRequest = wsettings_evt_setting,
            .ValueCb = sizeof(struct ozzy_ksproperty_setting),
        },
    };
    return AcxCircuitInitAssignProperties(init, properties, ARRAYSIZE(properties));
}
