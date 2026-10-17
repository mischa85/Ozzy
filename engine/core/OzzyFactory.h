#ifndef OzzyFactory_h
#define OzzyFactory_h

#include "OzzyEngine.h"
#include "../devices/ploytec/PloytecEngine.h"

class OzzyFactory {
public:
    static OzzyEngine* CreateEngine(uint16_t vendorID, uint16_t productID) {
        // Ploytec Devices
        if (vendorID == PLOYTEC_VENDOR_ID) {
            return new PloytecEngine(vendorID, productID);
        }
        
        // Future: Denon, Pioneer, etc.
        /*
        if (vendorID == 0x154F) {
            return new DenonEngine(productID);
        }
        */
        
        return nullptr;
    }
};

#endif