#include "KextBus.h"
#include "../../Shared/OzzyLog.h"
#include <stdarg.h>

// --- Context Wrapper (Keeps the SLICE alive) ---
struct TransferContext {
    IOMemoryDescriptor* slice; 
    void* userContext;       
    uint8_t pipeID;
    KextBus* busInstance;
};

// --- Global Callback ---
void KextBus_GlobalCompletion(void* owner, void* parameter, IOReturn status, uint32_t bytesTransferred) {
    TransferContext* ctx = (TransferContext*)parameter;
    if (!ctx) {
        LogOzzyKext("callback: NULL context");
        return;
    }

    KextBus* bus = ctx->busInstance;

    /*
    // --- DIAGNOSTIC LOGGING ---
    static int sTotalCallbacks = 0;
    sTotalCallbacks++;

    // Log the first 50 callbacks of ANY kind immediately
    if (sTotalCallbacks < 50) {
        LogOzzyKext("Callback #%d | Pipe: 0x%02X | Status: 0x%x | Bytes: %u", 
              sTotalCallbacks, ctx->pipeID, status, bytesTransferred);
    }
    */
    // Log errors immediately
    if (status != kIOReturnSuccess && status != kIOReturnAborted) {
        LogOzzyKext("pipe 0x%02x error: 0x%x", ctx->pipeID, status);
    }
    // ---------------------------

    // 1. RELEASE SLICE (Zero Copy Cleanup)
    if (ctx->slice) {
        IODirection dir = (ctx->pipeID & 0x80) ? kIODirectionIn : kIODirectionOut;
        ctx->slice->complete(dir);
        ctx->slice->release();
    }

    // 2. Extract Info
    void* originalUserContext = ctx->userContext;
    uint8_t pipeID = ctx->pipeID;
    delete ctx; 

    /*
    // 3. Keep the Heartbeat for long-term monitoring
    if (status == kIOReturnSuccess && pipeID == 0x05) {
        if ((sTotalCallbacks % 4000) == 0) { 
            LogOzzyKext("Stream Running (Count: %d)", sTotalCallbacks);
        }
    }
    */

    // 4. NOTIFY ENGINE
    if (bus) {
        OzzyEngine* engine = bus->GetEngine();
        if (engine) {
            engine->OnPacketComplete(pipeID, originalUserContext, status, bytesTransferred);
        }
    }
}

// --- Class Implementation ---

KextBus::KextBus(IOUSBHostDevice* device, IOService* driver) 
    : mDevice(device), mDriver(driver), mPipes(nullptr), mPipeCount(0), mEngine(nullptr), mMainShmDesc(nullptr), mShmBase(0) {
}

KextBus::~KextBus() {}

void KextBus::SetPipePool(IOUSBHostPipe** pipes, int count) {
    mPipes = pipes;
    mPipeCount = count;
}

void KextBus::SetEngine(OzzyEngine* engine) {
    mEngine = engine;
}

// NEW: Store the Master Descriptor references
void KextBus::SetSharedMemory(IOBufferMemoryDescriptor* mainDesc, void* virtBase) {
    mMainShmDesc = mainDesc;
    mShmBase = (uintptr_t)virtBase;
}

OzzyEngine* KextBus::GetEngine() {
    return mEngine;
}

IOUSBHostPipe* KextBus::FindPipe(uint8_t address) {
    if (!mPipes) return nullptr;
    if (address == 0x05) return mPipes[0];
    if (address == 0x86) return mPipes[1];
    if (address == 0x83) return mPipes[2];
    return nullptr;
}

bool KextBus::SubmitUSBPacket(uint8_t pipeID, void* buffer, uint32_t size, void* contextID) {
    IOUSBHostPipe* pipe = FindPipe(pipeID);
    if (!pipe) return false;
    
    // Safety check: Ensure we have the master memory configured
    if (!mMainShmDesc || mShmBase == 0) return false;

    // 1. Calculate Offset (Zero Copy Logic)
    uintptr_t ptr = (uintptr_t)buffer;
    IOByteCount offset = ptr - mShmBase;

    IODirection dir = (pipeID & 0x80) ? kIODirectionIn : kIODirectionOut;
    
    // 2. Create Sub-Descriptor (The Slice)
    // Uses IOSubMemoryDescriptor to view existing memory. No allocations, no copies.
    IOSubMemoryDescriptor* slice = IOSubMemoryDescriptor::withSubRange(
        mMainShmDesc, offset, size, dir);
    
    if (!slice) return false;
    
    // 3. Prepare DMA
    slice->prepare(dir);

    // 4. Create Context (Keep 'slice' alive)
    TransferContext* ctx = new TransferContext;
    if (!ctx) { slice->complete(dir); slice->release(); return false; }
    
    ctx->slice = slice;
    ctx->userContext = contextID;
    ctx->pipeID = pipeID;
    ctx->busInstance = this;
    
    IOUSBHostCompletion completion;
    completion.owner = this;
    completion.action = &KextBus_GlobalCompletion;
    completion.parameter = ctx;
    
    // 5. Submit
    IOReturn err = pipe->io(slice, size, &completion, 0);
    
    if (err != kIOReturnSuccess) {
        slice->complete(dir);
        slice->release();
        delete ctx;
        return false;
    }
    
    return true;
}

bool KextBus::VendorRequest(uint8_t type, uint8_t req, uint16_t val, uint16_t idx, void* data, uint16_t len) {
    if (!mDevice || !mDriver) return false;
    
    StandardUSB::DeviceRequest dr;
    dr.bmRequestType = type;
    dr.bRequest = req;
    dr.wValue = val;
    dr.wIndex = idx;
    dr.wLength = len;
    
    uint32_t bytesTransferred = 0;
    
    // Use Raw Pointer (Fixed 0 Samplerate issue)
    IOReturn err = mDevice->deviceRequest(mDriver, dr, data, bytesTransferred, 5000);
    
    return (err == kIOReturnSuccess);
}

void KextBus::Sleep(uint32_t ms) { IOSleep(ms); }
uint64_t KextBus::GetTime() { return mach_absolute_time(); }

void KextBus::Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    IOLogv(fmt, args);
    va_end(args);
}

// Stub
void KextBus::StaticCompletion(void* owner, void* parameter, IOReturn status, uint32_t bytesTransferred) {}