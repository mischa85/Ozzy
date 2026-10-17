#include "Device.h"

extern "C" DRIVER_INITIALIZE DriverEntry;

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath) {
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, OzzyEvtDeviceAdd);
    WDFDRIVER driver;
    NTSTATUS status = WdfDriverCreate(driverObject, registryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, &driver);
    if (!NT_SUCCESS(status)) return status;

    ACX_DRIVER_CONFIG acx;
    ACX_DRIVER_CONFIG_INIT(&acx);
    return AcxDriverInitialize(driver, &acx);
}
