#include "KextBus.h"
#include "../../../engine/shared/ozzy_log.h"
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
        ozzy_log("callback: NULL context");
        return;
    }

    KextBus* bus = ctx->busInstance;

    /*
    // --- DIAGNOSTIC LOGGING ---
    static int sTotalCallbacks = 0;
    sTotalCallbacks++;

    // Log the first 50 callbacks of ANY kind immediately
    if (sTotalCallbacks < 50) {
        ozzy_log("Callback #%d | Pipe: 0x%02X | Status: 0x%x | Bytes: %u", 
              sTotalCallbacks, ctx->pipeID, status, bytesTransferred);
    }
    */
    // Log errors immediately
    if (status != kIOReturnSuccess && status != kIOReturnAborted) {
        ozzy_log("pipe 0x%02x error: 0x%x", ctx->pipeID, status);
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
            ozzy_log("Stream Running (Count: %d)", sTotalCallbacks);
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

// --- Isochronous transfers ---
struct IsocContext {
    IOMemoryDescriptor* slice;
    void* userContext;
    uint8_t pipeID;
    KextBus* busInstance;
    OzzyIsocPacket* packets;
    uint32_t count;
    IOUSBHostIsochronousTransaction transactions[kOzzyMaxIsocPackets];
};

/* Copy each transaction's result back to the engine's packets, then notify the engine */
static void KextBus_IsocCompletion(void* owner, void* parameter, IOReturn status,
                                   IOUSBHostIsochronousTransaction* transactions) {
    IsocContext* ctx = (IsocContext*)parameter;
    if (!ctx) return;

    if (status != kIOReturnSuccess && status != kIOReturnAborted)
        ozzy_log("isoc pipe 0x%02x error: 0x%x", ctx->pipeID, status);

    for (uint32_t i = 0; i < ctx->count; i++) {
        ctx->packets[i].actual = ctx->transactions[i].completeCount;
        ctx->packets[i].status = ctx->transactions[i].status;
        memcpy(&ctx->packets[i].time, &ctx->transactions[i].timeStamp, sizeof(uint64_t));
    }

    IODirection dir = (ctx->pipeID & 0x80) ? kIODirectionIn : kIODirectionOut;
    ctx->slice->complete(dir);
    ctx->slice->release();

    KextBus* bus = ctx->busInstance;
    void* userContext = ctx->userContext;
    uint8_t pipeID = ctx->pipeID;
    OzzyIsocPacket* packets = ctx->packets;
    uint32_t count = ctx->count;
    delete ctx;

    OzzyEngine* engine = bus ? bus->GetEngine() : nullptr;
    if (engine) engine->OnIsocComplete(pipeID, userContext, status, packets, count);
}

// --- Class Implementation ---

KextBus::KextBus(IOUSBHostDevice* device, IOService* driver) 
    : mDevice(device), mDriver(driver), mPipes(nullptr), mPipeCount(0), mEngine(nullptr), mMainShmDesc(nullptr), mShmBase(0) {
    for (int i = 0; i < kPipeSlots; i++) mPipeByAddress[i] = nullptr;
}

KextBus::~KextBus() {
    StopTimer();
    if (mTimer) thread_call_free(mTimer);
}

bool KextBus::StartTimer(uint32_t intervalUs) {
    if (!mTimer) mTimer = thread_call_allocate_with_priority(&KextBus::TimerFired, this, THREAD_CALL_PRIORITY_HIGH);
    if (!mTimer) return false;
    nanoseconds_to_absolutetime((uint64_t)intervalUs * 1000, &mTimerInterval);
    mTimerDeadline = mach_absolute_time() + mTimerInterval;
    mTimerOn = true;
    thread_call_enter_delayed(mTimer, mTimerDeadline);
    return true;
}

void KextBus::StopTimer() {
    mTimerOn = false;
    if (!mTimer) return;
    // The first wait lets a running call finish; it may have re-armed before it saw mTimerOn,
    // so cancel once more. Later calls see mTimerOn false and don't re-arm.
    thread_call_cancel_wait(mTimer);
    thread_call_cancel_wait(mTimer);
}

void KextBus::TimerFired(thread_call_param_t param, thread_call_param_t) {
    KextBus* bus = (KextBus*)param;
    if (!bus->mTimerOn) return;
    if (bus->mEngine) bus->mEngine->OnTimer();

    // Next deadline from the last one; if we fell behind, skip to the next one ahead
    uint64_t now = mach_absolute_time();
    bus->mTimerDeadline += bus->mTimerInterval;
    if (bus->mTimerDeadline <= now) bus->mTimerDeadline = now + bus->mTimerInterval;
    if (bus->mTimerOn) thread_call_enter_delayed(bus->mTimer, bus->mTimerDeadline);
}

/* Index pipes by endpoint address so SubmitUSBPacket finds them without searching */
void KextBus::SetPipePool(IOUSBHostPipe** pipes, int count) {
    mPipes = pipes;
    mPipeCount = count;
    for (int i = 0; i < kPipeSlots; i++) mPipeByAddress[i] = nullptr;
    for (int i = 0; i < count; i++) {
        if (!pipes[i]) continue;
        const StandardUSB::EndpointDescriptor* d = pipes[i]->getEndpointDescriptor();
        if (d) mPipeByAddress[PipeSlot(d->bEndpointAddress)] = pipes[i];
    }
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
    return mPipeByAddress[PipeSlot(address)];
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

/* Zero copy like SubmitUSBPacket: the transactions point into a slice of the shared memory.
 * A packet that runs past the end of the buffer wraps to its start (XHCI). */
bool KextBus::SubmitIsoc(uint8_t pipeID, void* buffer, uint32_t bufferSize,
                         OzzyIsocPacket* packets, uint32_t count, void* contextID) {
    IOUSBHostPipe* pipe = FindPipe(pipeID);
    if (!pipe || !mMainShmDesc || mShmBase == 0 || count == 0 || count > kOzzyMaxIsocPackets) return false;

    IODirection dir = (pipeID & 0x80) ? kIODirectionIn : kIODirectionOut;
    IOSubMemoryDescriptor* slice = IOSubMemoryDescriptor::withSubRange(
        mMainShmDesc, (uintptr_t)buffer - mShmBase, bufferSize, dir);
    if (!slice) return false;
    slice->prepare(dir);

    IsocContext* ctx = new IsocContext;
    if (!ctx) { slice->complete(dir); slice->release(); return false; }
    ctx->slice = slice;
    ctx->userContext = contextID;
    ctx->pipeID = pipeID;
    ctx->busInstance = this;
    ctx->packets = packets;
    ctx->count = count;
    for (uint32_t i = 0; i < count; i++) {
        IOUSBHostIsochronousTransaction& t = ctx->transactions[i];
        memset(&t, 0, sizeof(t));
        t.requestCount = packets[i].length;
        t.offset = packets[i].offset;
        t.options = (packets[i].offset + packets[i].length > bufferSize) ? kIsochronousTransactionOptionsWrap
                                                                        : kIsochronousTransactionOptionsNone;
    }

    IOUSBHostIsochronousTransactionCompletion completion;
    completion.owner = this;
    completion.action = &KextBus_IsocCompletion;
    completion.parameter = ctx;

    IOReturn err = pipe->io(slice, ctx->transactions, count, 0, kIsochronousTransferOptionsNone, &completion);
    if (err != kIOReturnSuccess) {
        ozzy_log("isoc pipe 0x%02x submit failed: 0x%x", pipeID, err);
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