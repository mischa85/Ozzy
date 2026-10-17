#include "OzzyEngine.h"
#include "../shared/ozzy_shared_data.h"

// The base class implementation is minimal because most logic is protocol-specific.
// However, we can implement shared helpers here if needed in the future.

// Currently, the Init method is inline in the header, and Start/Stop are pure virtual.
// If we add shared state management (e.g. tracking "isRunning"), it goes here.

// For now, this file exists to ensure the vtable is emitted and to provide a home 
// for any future shared logic (like generic ring buffer utilities).

void OzzyEngine::Init(IOzzyBus* bus, void* sharedMemory) {
    mBus = bus;
    mSharedMemory = sharedMemory;
    ozzy_settings_defaults(&mSettings);
    
    if (mBus) {
        mBus->Log("OzzyEngine Base Initialized");
    }
}

void OzzyEngine::PublishSettings(const struct ozzy_clock_status& clock) {
    auto* shm = (volatile ozzy_shared_memory*)mSharedMemory;
    if (!shm) return;
    uint32_t available = 0;
    for (unsigned int i = 0; i < OZZY_NUM_SETTINGS; i++) {
        if (HasSetting(i)) available |= 1u << i;
        shm->audio.settings.value[i] = mSettings.value[i];
    }
    shm->audio.settings.available = available;
    shm->audio.settings.lock_rate = clock.locked ? clock.lock_rate : 0;
    shm->audio.settings.clock_locked = clock.locked;
}

// We can provide a default "do nothing" implementation for callbacks 
// so derived classes only implement what they need.

/*
void OzzyEngine::OnPacketComplete(uint8_t pipeID, void* ctx, int status, uint32_t bytes) {
    // Default: Do nothing. 
    // Derived classes (PloytecEngine) override this to handle their specific pipes.
}
*/