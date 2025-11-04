#pragma once

#include <rosval.h>

namespace PCI{
    U32 ReadDword(U8 Bus, U8 Device, U8 Function, U8 Offset);
    void WriteDword(U8 Bus, U8 Device, U8 Function, U8 Offset, U32 Value);
    void ScanBus(U8 bus);
    void ScanAllBuses();
    VOID IntializePCIDrivers();
}