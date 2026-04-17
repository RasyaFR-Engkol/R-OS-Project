#define PRINTK_MODULE_NAME "XHCIPCI"
#include <rosval.h>
#include "xhci.hpp"
#include "xhci_isr.hpp"
#include <string.hpp>
#include "../kernel/filesys/iblockdevice.hpp"
#include "../kernel/dev/devicemanager.hpp"
#include "../kernel/filesys/devfs/devfs.hpp"

namespace xHCI {
    using namespace Printk;

    VOID RegisterController(U8 Bus, U8 Device, U8 Function, U8 MSICapOffset){
        if(g_xhci_controller_count >= XHCI_MAX_CONTROLLERS) {
            PrintkWrite(Printk::Level::LOG_ERR, " xHCI: Max controller limit reached, skipping registration of %d:%d:%d\n",
                (int)Bus, (int)Device, (int)Function);
            return;
        }

        // Handle potential 64-bit BAR for xHCI (commonly a 64-bit MMIO BAR)
        U32 Bar0LOW = PCIReadDword(Bus, Device, Function, 0x10);
        U32 Bar0HI  = PCIReadDword(Bus, Device, Function, 0x14);
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
        VOID *VirtAddr = MmVirtualAllocPages(MapPagesCount);
        if(!VirtAddr){
            PrintkWrite(Level::LOG_ERR, " Failed allocating virtual for XHCI\n");
            return;
        }

        // Avoid setting NX here (EFER.NXE may be clear during early boot).
        PFLAGS Flags = PAGE_PRESENT | PAGE_RW | PAGE_PCD;
        if(!MmMapPages(KernelPML4, RegPhysPage, (UPTR)VirtAddr, MapPagesCount, Flags)) {
            PrintkWrite(Level::LOG_ERR, " Failed mapping XHCI registers\n");
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

        { 
            U8 msix_offset = PCIFindCapatibility(Bus, Device, Function, 0x11); 
            if (msix_offset != 0) {
                //Write(Level::LOG_INFO, " xHCI: MSI-X Capability found at 0x%x. Attempting Enable...\n", msix_offset);
                U8 vec = PCIEnableMSIX(Bus, Device, Function, msix_offset, xHCI_InterruptHandler_C0_TopHalf);
                
                if (vec != 0) {
                    DRV.IntVector = vec;
                    //Write(Level::LOG_INFO, " xHCI: MSI-X Enabled! Vector 0x%x\n", vec);
                    goto interrupt_done;
                } else {
                    PrintkWrite(Level::LOG_ERR, " xHCI: MSI-X Enable Failed. Trying fallback...\n");
                }
            }
        } // Tambahkan kurung kurawal penutup "}" di sini. 
          // Variabel msix_offset & vec mati disini, jadi aman dilompati.

        // 2. TRY MSI (The Standard)
        // Tambahkan kurung kurawal pembuka "{" di sini
        {
            U8 msi_offset = PCI::FindCapability(Bus, Device, Function, 0x05); 
            if (msi_offset != 0) {
                //Write(Level::LOG_INFO, " xHCI: MSI Capability found at 0x%x. Attempting Enable...\n", msi_offset);
                U8 vec = PCIEnableMSI(Bus, Device, Function, msi_offset, xHCI_InterruptHandler_C0_TopHalf);
                
                if (vec != 0) {
                    DRV.IntVector = vec;
                    //Write(Level::LOG_INFO, " xHCI: MSI Enabled! Vector 0x%x\n", vec);
                    goto interrupt_done;
                }
            }
        } // Tambahkan kurung kurawal penutup "}" di sini

        // 3. TRY LEGACY INTx (The Old Reliable)
        {
            U8 irq = PCIEnableLegacyINTx(Bus, Device, Function, xHCI_InterruptHandler_C0);
            if (irq != 0) {
                DRV.IntVector = 0x20 + irq;
                Write(Level::LOG_WARNING, " xHCI: Fallback to Legacy INTx IRQ %d (Vec 0x%x)\n", irq, DRV.IntVector);
            } else {
                Write(Level::LOG_ERR, " xHCI: FATAL - No Interrupt method available!\n");
            }
        }

        interrupt_done:

        // Kita udah set interrupt. saatnya daftarin threaded IRQ handler-nya
        if (DRV.IntVector != 0) {
            RequestThreadedIrq(DRV.IntVector, xHCI_InterruptHandler_C0_TopHalf, xHCI_Worker_Thread, (VOID*)(UPTR)g_xhci_controller_count);
            PrintkWrite(Level::LOG_INFO, " Registered threaded IRQ handler for vector 0x%x\n", DRV.IntVector);
        } else {
            PrintkWrite(Level::LOG_ERR, " Failed to register any interrupt handler for this controller!\n");
        }

        g_xhci_controller_count++;

        // Create a small IBlockDevice wrapper so the controller can be
        // exposed via /dev as a device node (name format: "ud%c"). The
        // wrapper does not implement sector IO; it simply provides a name
        // so DevFS/DeviceManager can list the controller.

        // Instantiate and register the device
        int idx_for_name = g_xhci_controller_count - 1;
        XHCIBlockDevice *dev = new XHCIBlockDevice(idx_for_name);
        if(!dev) {
            PrintkWrite(Level::LOG_ERR, " Failed allocating XHCI dev wrapper\n");
        } else {
            VFSCreateBlockNode(dev);
        }
    }
}
