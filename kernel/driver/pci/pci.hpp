#pragma once

#include <rosval.h>

namespace PCI{
    U32 ReadDword(U8 Bus, U8 Device, U8 Function, U8 Offset);
    void WriteDword(U8 Bus, U8 Device, U8 Function, U8 Offset, U32 Value);
    U16 ReadWord(U8 Bus, U8 Device, U8 Function, U8 Offset);
    void WriteWord(U8 Bus, U8 Device, U8 Function, U8 Offset, U16 Value);
    U8 ReadByte(U8 Bus, U8 Device, U8 Function, U8 Offset);
    void WriteByte(U8 Bus, U8 Device, U8 Function, U8 Offset, U8 Value);

    void ScanBus(U8 bus);
    void ScanAllBuses();
    VOID IntializePCIDrivers();
    // Enable legacy INTx for a device and register an IRQ handler.
    // Handler receives a void* context parameter. Returns the IRQ number
    // (0..255) on success, 0 on failure.
    U8 EnableLegacyINTxForDevice(U8 Bus, U8 Device, U8 Function, void (*irq_handler)(void *context));
    U8 FindCapability(U8 Bus, U8 Device, U8 Function, U8 TargetCapID);
}

// STRUCT KECIL KECILAN
struct pci_data{
    U8 bus;
    U8 device;
    U8 function;
    U8 msix_offset;
};