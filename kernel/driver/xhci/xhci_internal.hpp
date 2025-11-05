#pragma once

#include "xhci.hpp"

// Internal helpers shared across xHCI modules (not part of public API)
namespace xHCI {
    // Command submission helpers
    VOID SendNOOPCommand(xHCIDriver &DRV);
    VOID SendEnableSlotCommand(xHCIDriver &DRV);

    // Diagnostics
    void DumpXHCIState(xHCIDriver &DRV, const char* tag);
}
