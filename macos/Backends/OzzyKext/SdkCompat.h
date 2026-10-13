#pragma once

#include <stdint.h>

/*
 * The macOS 27 SDK moved the driver power-assertion typedefs out of
 * IOKit/pwr_mgt/RootDomain.h into IOPMAssertionTypes.h, but that header
 * ships empty, so RootDomain.h and IOUSBHostDevice.h fail to compile.
 * Restate the definitions from the 26.5 SDK's RootDomain.h; repeating an
 * identical typedef is legal, so this stays harmless once the SDK is fixed.
 */
typedef uint64_t IOPMDriverAssertionID;
typedef uint64_t IOPMDriverAssertionType;
typedef uint32_t IOPMDriverAssertionLevel;
