#pragma once
#include "ahci.hpp"
#include "string.hpp"

#define ATA_CMD_FLUSH_CACHE_EXT 0xEA

namespace AHCI {
    // Globals shared across AHCI compilation units
    extern AHCIDriver g_ahci_controllers[MAX_AHCI_CONTROLLERS];
    extern int g_ahci_controller_count;

    // ISR handler table (defined in interrupts.cpp). Handlers now accept
    // a void* context parameter per the new interrupt API.
    extern void (*g_ahci_handlers[] )(void *);

    // Forward declarations for functions split into separate translation units
    VOID HandleInterrupt(VAL32 Controller_ID);

    BOOL InitializePort(AHCIDriver &Driver, int NumPort);
    DeviceType ProbePort(AHCIDriver &Drv, VAL32 PortNum);
    BOOL SendIdentify(AHCIDriver &Driver, VAL32 PortNum);

    VAL32 FindFreeCommandSlot(AHCIDriver &Driver, VAL32 PortNum);
    BOOL ReadSectors(AHCIDriver &Driver, VAL32 PortNum, U64 lba, U32 count,
                     PageAlloc::DMAAlloc::DMABuffer **outBuf);
    BOOL WriteSectors(AHCIDriver &Driver, VAL32 PortNum, U64 lba, U32 count,
                      PageAlloc::DMAAlloc::DMABuffer *buf);
    VOID TestReadLBA0();
    BOOL FlushCache(AHCIDriver &Driver, VAL32 PortNum);
}