#ifndef IOzzyBus_h
#define IOzzyBus_h

#include <stdint.h>

class OzzyEngine;

#define kOzzyMaxIsocPackets 32   // packets per isochronous transfer (4 ms of microframes)

// One packet (service interval) of an isochronous transfer. The engine fills offset and
// length; the bus fills actual, status and time (mach absolute time) on completion.
struct OzzyIsocPacket {
    uint32_t offset;   // in the transfer's buffer; may run past its end and wrap to its start
    uint32_t length;
    uint32_t actual;
    int32_t  status;
    uint64_t time;
};

class IOzzyBus {
public:
    virtual ~IOzzyBus() {}

    // --- Core Transport ---
    // Returns true if successfully queued.
    // contextID: A generic tag (usually packet index) passed back in OnPacketComplete
    virtual bool SubmitUSBPacket(uint8_t pipeID, void* buffer, uint32_t size, void* contextID) = 0;

    // Isochronous transfer of @count packets (at most kOzzyMaxIsocPackets) over @buffer, which
    // is @bufferSize bytes; starts at the next frame. @packets must stay valid until
    // OzzyEngine::OnIsocComplete.
    virtual bool SubmitIsoc(uint8_t pipeID, void* buffer, uint32_t bufferSize,
                            OzzyIsocPacket* packets, uint32_t count, void* contextID) { return false; }
    
    // Blocking Control Request (Used for Initialization)
    // type: bmRequestType, req: bRequest
    virtual bool VendorRequest(uint8_t type, uint8_t req, uint16_t val, uint16_t idx, void* data, uint16_t len) = 0;

    // Call OzzyEngine::OnTimer() every @intervalUs until StopTimer() (one timer per bus).
    // StopTimer() waits for a call in progress.
    virtual bool StartTimer(uint32_t intervalUs) { return false; }
    virtual void StopTimer() {}

    // --- System Services ---
    virtual void Sleep(uint32_t ms) = 0;
    virtual uint64_t GetTime() = 0; // Returns mach_absolute_time()
    virtual void Log(const char* fmt, ...) = 0;
    
    // --- Linkage ---
    virtual void SetEngine(OzzyEngine* engine) = 0;
};

#endif
