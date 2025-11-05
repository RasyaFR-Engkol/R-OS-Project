#include <rosval.h>
#include "ahci.hpp"
#include "ahci_regs.hpp"
#include "rossys.hpp"
#include "string.hpp"
#include <port.hpp>
#include <mm.hpp>
#include <logging.hpp>
#include "../pic/timer/pit.hpp"
#include "../pci/capatibility/msixmsi/msixmsi.hpp"
#include "ahci_internal.hpp"

namespace AHCI {
    // Core AHCI controller globals remain here; other translation units
    // will reference these via the internal header.
    AHCIDriver g_ahci_controllers[MAX_AHCI_CONTROLLERS];
    int g_ahci_controller_count = 0;

    // The ISR handlers table is defined in interrupts.cpp. Declare it as
    // extern here so RegisterAHCIController can reference the handler.
    extern void (*g_ahci_handlers[])();

    VOID RegisterAHCIController(U8 Bus, U8 Device, U8 Function, U8 MSICapOffset){
        if(g_ahci_controller_count >= MAX_AHCI_CONTROLLERS){
            return;
        }

        U32 ABAR_RAW = PCI::ReadDword(Bus, Device, Function, 0x24);
        UPTR ABAR_Phys = (UPTR)(ABAR_RAW & 0xFFFFFFF0);
        UPTR ABAR_PhysPage = ABAR_Phys & ~(PAGE_SIZE - 1);

        VOID* VirtAddr = PageAlloc::VirtualAllocPages(1);
        if(!VirtAddr) {
            return;
        }

        PFLAGS Flags = PAGE_PRESENT | PAGE_RW | PAGE_PCD;
        if(!PageAlloc::MapPages(KernelPML4, ABAR_PhysPage, (UPTR)VirtAddr, 1, Flags)) {
            return;
        }

        LOCKRFLAGS _ahci_rflags = Arch::SaveAndDisableInterrupts();
        AHCIDriver &DRV = g_ahci_controllers[g_ahci_controller_count];
        DRV.regs = (volatile HBA_MEM*)VirtAddr;
        DRV.bus = Bus;
        DRV.dev = Device;
        DRV.func = Function;
        DRV.initialized = FALSE;
        DRV.IntVector = 0;

        if(MSICapOffset != 0){
            VOID (*MyHandler)(VOID) = g_ahci_handlers[g_ahci_controller_count];

            U8 Vector = MSI::EnableMSI(Bus, Device, Function, MSICapOffset, MyHandler);
            if(Vector != 0){
                DRV.IntVector = Vector;
                Printk::Write(Printk::Level::LOG_INFO, "[AHCI] Enabled MSI on AHCI Controller %02X:%02X:%02X with vector 0x%02x\n",
                    (unsigned)Bus, (unsigned)Device, (unsigned)Function, (unsigned)Vector);
            } else {
                Printk::Write(Printk::Level::LOG_ERR, "[AHCI] Failed to enable MSI on AHCI Controller %02X:%02X:%02X\n",
                    (unsigned)Bus, (unsigned)Device, (unsigned)Function);
            }
        }

        g_ahci_controller_count++;

        Arch::RestoreInterrupts(_ahci_rflags);

        Printk::Write(Printk::Level::LOG_INFO, "[AHCI] Registered AHCI Controller at %02X:%02X:%02X, ABAR phys=%p virt=%p\n",
            (unsigned)Bus, (unsigned)Device, (unsigned)Function,
            (void*)(uintptr_t)ABAR_Phys, VirtAddr);
    }

    VOID InitializeAllControllers() {
        Printk::Write(Printk::Level::LOG_NOTICE, "[AHCI] Initializing all AHCI controllers (%d found)\n", g_ahci_controller_count);

        for(int i = 0; i < g_ahci_controller_count; i++) {
            AHCIDriver &DRV = g_ahci_controllers[i];

            // PASTIKAN SAKLAR GLOBAL MATI DULU
            DRV.regs->ghc &= ~(1 << 1); // GHC.IE = 0

            U32 PortImplemented = DRV.regs->pi;

            Printk::Write(Printk::Level::LOG_INFO, "[AHCI] Controller %d at %02X:%02X:%02X - Ports Implemented: 0x%08X\n",
                i, (unsigned)DRV.bus, (unsigned)DRV.dev, (unsigned)DRV.func,
                (unsigned)PortImplemented);

            for(int portnum = 0; portnum < 32; portnum++) {
                if(PortImplemented & (1 << portnum)) {
                    if(InitializePort(DRV, portnum)) {
                        DeviceType DevType = ProbePort(DRV, portnum);
                        if(DevType == DeviceType::SATA) {
                            if (DRV.IntVector != 0) {
                                // INI DIA! HANYA AKTIFKAN 'ie' UNTUK PORT INI
                                volatile HBA_PORT* port = &DRV.regs->ports[portnum];
                                port->ie = (1u << 0) | (1u << 30); // D2H + Fatal Error
                            }
                            SendIdentify(DRV, portnum);
                        }
                        DRV.port_device[portnum] = DevType;
                    } else {
                        Printk::Write(Printk::Level::LOG_ERR, "[AHCI] Controller %d Port %d failed to initialize\n", i, portnum);
                    }
                }
            }

            DRV.initialized = TRUE;
            // --- TAMBAHKAN BLOK INI ---
            // Setelah semua port di-init, nyalakan Global Interrupt Enable
            if (DRV.IntVector != 0) {
                DRV.regs->ghc |= (1 << 1); // Set GHC.IE (bit 1)
            }
            // -------------------------
        }
    }
}