// new/delete for the engine and driver objects: nonpaged pool, zeroed (ExAllocatePool2), which
// the engines expect (the kext also zeroes them). No exceptions: allocation failure is nullptr.
#include "Device.h"

void* __cdecl operator new(size_t size) {
    return ExAllocatePool2(POOL_FLAG_NON_PAGED, size, OZZY_POOL_TAG);
}

void __cdecl operator delete(void* p) noexcept {
    if (p) ExFreePoolWithTag(p, OZZY_POOL_TAG);
}

void __cdecl operator delete(void* p, size_t) noexcept {
    if (p) ExFreePoolWithTag(p, OZZY_POOL_TAG);
}
