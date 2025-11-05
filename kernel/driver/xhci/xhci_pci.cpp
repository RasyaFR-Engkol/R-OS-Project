#include <rosval.h>
#include <logging.hpp>
#include <drivers/pci.hpp>
#include <mm.hpp>
#include "xhci.hpp"
#include "xhci_isr.hpp"

namespace xHCI {
    using namespace Printk;

    VOID RegisterController(U8 Bus, U8 Device, U8 Function, U8 MSICapOffset){
        if(g_xhci_controller_count >= XHCI_MAX_CONTROLLERS) {
            Write(Level::LOG_ERR, "[XHCI] Controller full\n");
        }

        // Handle potential 64-bit BAR for xHCI (commonly a 64-bit MMIO BAR)
        U32 Bar0LOW = PCI::ReadDword(Bus, Device, Function, 0x10);
        U32 Bar0HI  = PCI::ReadDword(Bus, Device, Function, 0x14);
        U64 BarType = (Bar0LOW & 0x6) >> 1; // 0=32-bit, 2=64-bit
        U64 RegPhys64 = (BarType == 0x2)
            ? (((U64)Bar0HI << 32) | (U64)(Bar0LOW & 0xFFFFFFF0))
            : (U64)(Bar0LOW & 0xFFFFFFF0);
        UPTR RegPhys = (UPTR)RegPhys64;
        // Align physical base down to page boundary before mapping
        UPTR RegPhysPage = RegPhys & ~(PAGE_SIZE - 1);

        // XHCI register space can span multiple pages (capability, op, runtime,
        // doorbells). Map a larger window (e.g. 16 pages = 64KiB) to cover
        // typical controllers instead of a single page which caused PFs.
        const SIZE_T MapPagesCount = 16;
        VOID *VirtAddr = PageAlloc::VirtualAllocPages(MapPagesCount);
        if(!VirtAddr){
            Write(Level::LOG_ERR, "[XHCI] Failed allocating virtual for XHCI\n");
            return;
        }

        // Avoid setting NX here (EFER.NXE may be clear during early boot).
        PFLAGS Flags = PAGE_PRESENT | PAGE_RW | PAGE_PCD;
        if(!PageAlloc::MapPages(KernelPML4, RegPhysPage, (UPTR)VirtAddr, MapPagesCount, Flags)) {
            Write(Level::LOG_ERR, "[XHCI] Failed mapping XHCI registers\n");
            return;
        }

        UPTR PageOffset = RegPhys & (PAGE_SIZE - 1);

        xHCIDriver &DRV = g_xhci_controllers[g_xhci_controller_count];
        DRV.bus = Bus;
        DRV.dev = Device;
        DRV.func = Function;
        DRV.regs_base = (volatile U8*)VirtAddr + PageOffset;
        DRV.Initialized = FALSE;
        DRV.IntVector = 0;

        if(MSICapOffset != 0){
            // Hook controller 0 to ISR for now
            U8 Vector = MSI::EnableMSI(Bus, Device, Function, MSICapOffset, xHCI_InterruptHandler_C0);
            if(Vector != 0){
                DRV.IntVector = Vector;
                Write(Level::LOG_INFO, "[XHCI] Enabled MSI on XHCI Controller %02X:%02X:%02X with vector 0x%02x\n",
                    (unsigned)Bus, (unsigned)Device, (unsigned)Function, (unsigned)Vector);
            } else {
                Write(Level::LOG_ERR, "[XHCI] Failed to enable MSI on XHCI Controller %02X:%02X:%02X\n",
                    (unsigned)Bus, (unsigned)Device, (unsigned)Function);
            }
        }

        g_xhci_controller_count++;

        Write(Level::LOG_INFO, "[XHCI] Registered XHCI Controller %02X:%02X:%02X\n",
            (unsigned)Bus, (unsigned)Device, (unsigned)Function);

    }
}
