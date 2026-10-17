#pragma once
#include "../../core/OzzyEngine.h" // Includes IOzzyBus definition

class PloytecBus : public IOzzyBus {
public:
    virtual ~PloytecBus() {}
    
    // We inherit the pure virtuals from IOzzyBus:
    // SubmitUSBPacket, VendorRequest, Sleep, GetTime, Log, SetEngine
    // We don't need to redeclare them here if we don't implement them here.
};