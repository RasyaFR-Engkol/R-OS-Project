#pragma once

#include "xhci.hpp"

// Internal helpers shared across xHCI modules (not part of public API)
namespace xHCI {
    // Command submission helpers
    VOID SendNOOPCommand(xHCIDriver &DRV);
    VOID SendEnableSlotCommand(xHCIDriver &DRV);

    // Diagnostics
    void DumpXHCIState(xHCIDriver &DRV, const char* tag);
    // Helper: map a completed SlotID back to the root port that requested it
    U8 GetPortIDForSlot(xHCIDriver &DRV, U8 SlotID);

    VOID SetDeviceConfiguration(xHCIDriver &DRV, U8 SlotID, U8 ConfigValue);
}
