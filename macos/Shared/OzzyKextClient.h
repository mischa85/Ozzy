#ifndef OzzyKextClient_h
#define OzzyKextClient_h

// Helpers for the user-space clients of OzzyKext (OzzyHAL, OzzyMIDI)

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

/* The USB port of the device an OzzyKext instance drives (its provider's locationID).
 * Ploytec devices have no serial number, so this is what tells two of the same model apart. */
static inline uint32_t OzzyUSBLocationID(io_service_t service) {
    uint32_t locationID = 0;
    CFTypeRef value = IORegistryEntrySearchCFProperty(service, kIOServicePlane, CFSTR("locationID"),
        kCFAllocatorDefault, kIORegistryIterateRecursively | kIORegistryIterateParents);
    if (value) {
        if (CFGetTypeID(value) == CFNumberGetTypeID())
            CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, &locationID);
        CFRelease(value);
    }
    return locationID;
}

#endif
