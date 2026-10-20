#include "KextBus.h"
#include "../../../engine/shared/ozzy_log.h"
#include <stdarg.h>
#include <Availability.h>
#include <IOKit/IOMultiMemoryDescriptor.h>
#include <libkern/version.h>

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
        struct ozzy_engine* engine = bus->GetEngine();
        if (engine) {
            ozzy_engine_complete(engine, pipeID, originalUserContext, status, bytesTransferred);
        }
    }
}

// --- Isochronous transfers ---
/* macOS 12 added the transaction io() (IOUSBHostPipe slot 50): every packet has its own offset
 * and may wrap. Older kernels only have the frame io() (deprecated since), whose frames sit back
 * to back in the memory descriptor. Calling slot 50 on an older kernel panics, so the transaction
 * io() needs both an SDK that declares it and a kernel that has it (Darwin 21). */
static bool UseIsocTransactions() {
#ifdef __MAC_12_0
    return version_major >= 21;
#else
    return false;
#endif
}

struct IsocContext {
    IOMemoryDescriptor* slice;
    void* userContext;
    uint8_t pipeID;
    KextBus* busInstance;
    struct ozzy_isoc_packet* packets;
    uint32_t count;
    union {
#ifdef __MAC_12_0
        IOUSBHostIsochronousTransaction transactions[OZZY_MAX_ISOC_PACKETS];
#endif
        IOUSBHostIsochronousFrame frames[OZZY_MAX_ISOC_PACKETS];
    };
};

/* Release the slice, then hand the packets (results copied in) to the engine */
static void KextBus_IsocFinish(IsocContext* ctx, IOReturn status) {
    if (status != kIOReturnSuccess && status != kIOReturnAborted)
        ozzy_log("isoc pipe 0x%02x error: 0x%x", ctx->pipeID, status);

    IODirection dir = (ctx->pipeID & 0x80) ? kIODirectionIn : kIODirectionOut;
    ctx->slice->complete(dir);
    ctx->slice->release();

    KextBus* bus = ctx->busInstance;
    void* userContext = ctx->userContext;
    uint8_t pipeID = ctx->pipeID;
    struct ozzy_isoc_packet* packets = ctx->packets;
    uint32_t count = ctx->count;
    delete ctx;

    struct ozzy_engine* engine = bus ? bus->GetEngine() : nullptr;
    if (engine) ozzy_engine_isoc_complete(engine, pipeID, userContext, status, packets, count);
}

#ifdef __MAC_12_0
/* Copy each transaction's result back to the engine's packets */
static void KextBus_IsocCompletion(void* owner, void* parameter, IOReturn status,
                                   IOUSBHostIsochronousTransaction* transactions) {
    IsocContext* ctx = (IsocContext*)parameter;
    if (!ctx) return;
    for (uint32_t i = 0; i < ctx->count; i++) {
        ctx->packets[i].actual = ctx->transactions[i].completeCount;
        ctx->packets[i].status = ctx->transactions[i].status;
        memcpy(&ctx->packets[i].time, &ctx->transactions[i].timeStamp, sizeof(uint64_t));
    }
    KextBus_IsocFinish(ctx, status);
}
#endif

/* Copy each frame's result back to the engine's packets */
static void KextBus_IsocFrameCompletion(void* owner, void* parameter, IOReturn status,
                                        IOUSBHostIsochronousFrame* frames) {
    IsocContext* ctx = (IsocContext*)parameter;
    if (!ctx) return;
    for (uint32_t i = 0; i < ctx->count; i++) {
        ctx->packets[i].actual = ctx->frames[i].completeCount;
        ctx->packets[i].status = ctx->frames[i].status;
        memcpy(&ctx->packets[i].time, &ctx->frames[i].timeStamp, sizeof(uint64_t));
    }
    KextBus_IsocFinish(ctx, status);
}

// --- Class Implementation ---

// --- struct ozzy_bus_ops: the engine's calls, to the methods ---
static bool OpsSubmit(void* ctx, uint8_t pipe, void* buf, uint32_t size, void* xfer) {
    return ((KextBus*)ctx)->SubmitUSBPacket(pipe, buf, size, xfer);
}
static bool OpsSubmitIsoc(void* ctx, uint8_t pipe, void* buf, uint32_t bufSize, struct ozzy_isoc_packet* packets,
                          uint32_t count, void* xfer) {
    return ((KextBus*)ctx)->SubmitIsoc(pipe, buf, bufSize, packets, count, xfer);
}
static bool OpsControl(void* ctx, uint8_t type, uint8_t req, uint16_t val, uint16_t idx, void* data, uint16_t len) {
    return ((KextBus*)ctx)->VendorRequest(type, req, val, idx, data, len);
}
static bool OpsStartTimer(void* ctx, uint32_t intervalUs) { return ((KextBus*)ctx)->StartTimer(intervalUs); }
static void OpsStopTimer(void* ctx) { ((KextBus*)ctx)->StopTimer(); }
static void OpsSleep(void* ctx, unsigned int ms) { ((KextBus*)ctx)->Sleep(ms); }
static uint64_t OpsTime(void* ctx) { return ((KextBus*)ctx)->GetTime(); }

// No PCM rings on macOS: the HAL writes the shared memory itself
const struct ozzy_bus_ops KextBus::sOps = {
    OpsSubmit, OpsSubmitIsoc, OpsControl, OpsStartTimer, OpsStopTimer, nullptr, OpsSleep, OpsTime
};

KextBus::KextBus(IOUSBHostDevice* device, IOService* driver) 
    : mDevice(device), mDriver(driver), mPipes(nullptr), mPipeCount(0), mEngine(nullptr), mMainShmDesc(nullptr), mShmBase(0) {
    for (int i = 0; i < kPipeSlots; i++) mPipeByAddress[i] = nullptr;
    for (int i = 0; i < kPipeSlots; i++) mIsocNextFrame[i] = 0;
    mOzzyBus.ops = &sOps;
    mOzzyBus.ctx = this;
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
    if (bus->mEngine) ozzy_engine_timer(bus->mEngine);

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

void KextBus::SetEngine(struct ozzy_engine* engine) {
    mEngine = engine;
}

// NEW: Store the Master Descriptor references
void KextBus::SetSharedMemory(IOBufferMemoryDescriptor* mainDesc, void* virtBase) {
    mMainShmDesc = mainDesc;
    mShmBase = (uintptr_t)virtBase;
}

struct ozzy_engine* KextBus::GetEngine() {
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

/* Zero copy like SubmitUSBPacket: the packets point into a slice of the shared memory.
 * A packet that runs past the end of the buffer wraps to its start (XHCI). */
bool KextBus::SubmitIsoc(uint8_t pipeID, void* buffer, uint32_t bufferSize,
                         struct ozzy_isoc_packet* packets, uint32_t count, void* contextID) {
    IOUSBHostPipe* pipe = FindPipe(pipeID);
    if (!pipe || !mMainShmDesc || mShmBase == 0 || count == 0 || count > OZZY_MAX_ISOC_PACKETS) return false;

    IODirection dir = (pipeID & 0x80) ? kIODirectionIn : kIODirectionOut;
    bool transactions = UseIsocTransactions();
    IOMemoryDescriptor* slice = transactions
        ? IOSubMemoryDescriptor::withSubRange(mMainShmDesc, (uintptr_t)buffer - mShmBase, bufferSize, dir)
        : IsocFrameSlice(pipeID, buffer, bufferSize, packets, count, dir);
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

    uint64_t frame = IsocFrameNumber(pipe, pipeID, count);
    IOReturn err;
#ifdef __MAC_12_0
    if (transactions) {
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
        err = pipe->io(slice, ctx->transactions, count, frame, kIsochronousTransferOptionsNone, &completion);
    } else
#endif
    {
        for (uint32_t i = 0; i < count; i++) {
            IOUSBHostIsochronousFrame& f = ctx->frames[i];
            memset(&f, 0, sizeof(f));
            f.requestCount = packets[i].length;
        }

        IOUSBHostIsochronousCompletion completion;
        completion.owner = this;
        completion.action = &KextBus_IsocFrameCompletion;
        completion.parameter = ctx;
        err = pipe->io(slice, ctx->frames, count, frame, &completion);
    }
    if (err != kIOReturnSuccess) {
        ozzy_log("isoc pipe 0x%02x submit failed: 0x%x", pipeID, err);
        mIsocNextFrame[PipeSlot(pipeID)] = frame;
        slice->complete(dir);
        slice->release();
        delete ctx;
        return false;
    }
    return true;
}

/* Where an isochronous transfer of @count packets starts, for either io(). Frame number 0 (as soon
 * as possible) is XHCI only, so like the EMU USB audio kext each transfer starts where the pipe's
 * previous one ended, the first kIsocStartFrames ahead of the bus. Frame numbers count 1 ms
 * frames, a packet is one service interval (a microframe at High Speed bInterval 1); the engine's
 * transfers fill whole frames (ISOC_TRANSFER_UFRAMES). */
uint64_t KextBus::IsocFrameNumber(IOUSBHostPipe* pipe, uint8_t pipeID, uint32_t count) {
    const StandardUSB::EndpointDescriptor* d = pipe->getEndpointDescriptor();
    uint32_t intervals = count << ((d && d->bInterval ? d->bInterval : 1) - 1);
    uint64_t frames = mDevice->getSpeed() >= kUSBHostConnectionSpeedHigh ? (intervals + 7) / 8 : intervals;

    uint64_t now = mDevice->getFrameNumber();
    uint64_t& next = mIsocNextFrame[PipeSlot(pipeID)];
    if (next <= now) next = now + kIsocStartFrames;  /* the first transfer, or the pipe ran dry */
    uint64_t first = next;
    next += frames;
    return first;
}

/* The frame io() reads the frames back to back from the descriptor, so it gets exactly the
 * packets' bytes: from the first packet's offset on, continued at the buffer's start if they
 * wrap (the ring's tail and head as one descriptor). Fails for packets that aren't contiguous. */
IOMemoryDescriptor* KextBus::IsocFrameSlice(uint8_t pipeID, void* buffer, uint32_t bufferSize,
                                            const struct ozzy_isoc_packet* packets, uint32_t count,
                                            IODirection dir) {
    uint32_t start = packets[0].offset;
    uint32_t total = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (packets[i].offset != (start + total) % bufferSize) {
            ozzy_log("isoc pipe 0x%02x: packet %u not contiguous, the frame io() can't send it", pipeID, i);
            return nullptr;
        }
        total += packets[i].length;
    }
    if (start >= bufferSize || total > bufferSize) return nullptr;

    IOByteCount base = (uintptr_t)buffer - mShmBase;
    if (total == 0)  /* only empty frames: they read nothing, any descriptor will do */
        return IOSubMemoryDescriptor::withSubRange(mMainShmDesc, base, bufferSize, dir);
    if (start + total <= bufferSize)
        return IOSubMemoryDescriptor::withSubRange(mMainShmDesc, base + start, total, dir);

    IOMemoryDescriptor* parts[2] = {
        IOSubMemoryDescriptor::withSubRange(mMainShmDesc, base + start, bufferSize - start, dir),
        IOSubMemoryDescriptor::withSubRange(mMainShmDesc, base, start + total - bufferSize, dir),
    };
    IOMemoryDescriptor* slice = (parts[0] && parts[1])
        ? IOMultiMemoryDescriptor::withDescriptors(parts, 2, dir, false) : nullptr;
    if (parts[0]) parts[0]->release();
    if (parts[1]) parts[1]->release();
    return slice;
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

// Stub
void KextBus::StaticCompletion(void* owner, void* parameter, IOReturn status, uint32_t bytesTransferred) {}