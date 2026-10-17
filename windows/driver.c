#include "device.h"

DRIVER_INITIALIZE DriverEntry;

NTSTATUS DriverEntry(PDRIVER_OBJECT driver_object, PUNICODE_STRING registry_path) {
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, wdev_evt_add);
    WDFDRIVER driver;
    NTSTATUS status = WdfDriverCreate(driver_object, registry_path, WDF_NO_OBJECT_ATTRIBUTES, &config, &driver);
    if (!NT_SUCCESS(status)) return status;

    ACX_DRIVER_CONFIG acx;
    ACX_DRIVER_CONFIG_INIT(&acx);
    return AcxDriverInitialize(driver, &acx);
}
