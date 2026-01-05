#define PRINTK_MODULE_NAME "AHCI"
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
#include "ahci_disk.hpp"
#include "../../filesys/iblockdevice.hpp"
#include "../../dev/devicemanager.hpp"
// Access IOAPIC helpers
#include "../../../firmware/acpi/madt/madt.hpp"
#include "../../../firmware/acpi/madt/madt.hpp"

    /* module name provided via PRINTK_MODULE_NAME */

namespace AHCI {

    Mutex DiskLock;
    // Core AHCI controller globals remain here; other translation units
    // will reference these via the internal header.
    AHCIDriver g_ahci_controllers[MAX_AHCI_CONTROLLERS];
    int g_ahci_controller_count = 0;

    // The ISR handlers table is defined in interrupts.cpp. Declare it as
    // extern here so RegisterAHCIController can reference the handler.
    extern void (*g_ahci_handlers[])(void *);

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
            // Handlers now take a void* context
            VOID (*MyHandler)(void *) = g_ahci_handlers[g_ahci_controller_count];

            U8 Vector = MSI::EnableMSI(Bus, Device, Function, MSICapOffset, MyHandler);
            if(Vector != 0){
                DRV.IntVector = Vector;
                DRV.using_msi = TRUE;
                DRV.msi_cap_offset = MSICapOffset;
            } else {
                Printk::Write(Printk::Level::LOG_ERR, " Failed to enable MSI on AHCI Controller %02X:%02X:%02X\n",
                    (unsigned)Bus, (unsigned)Device, (unsigned)Function);
            }
        } else {
            // Try legacy INTx fallback. Pass handler with new signature.
            VOID (*MyHandler)(void *) = g_ahci_handlers[g_ahci_controller_count];
            U8 irq = PCI::EnableLegacyINTxForDevice(Bus, Device, Function, MyHandler);
            if (irq != 0) {
                DRV.IntVector = (U8)(0x20 + irq);
                DRV.using_msi = FALSE;
                DRV.legacy_irq = irq;
                Printk::Write(Printk::Level::LOG_DEBUG, " Enabled legacy INTx IRQ %u for AHCI Controller %02X:%02X:%02X (vector 0x%02x)\n",
                    (unsigned)irq, (unsigned)Bus, (unsigned)Device, (unsigned)Function, (unsigned)DRV.IntVector);
            } else {
                Printk::Write(Printk::Level::LOG_WARNING, " No MSI and legacy INTx unavailable for AHCI %02X:%02X:%02X\n",
                    (unsigned)Bus, (unsigned)Device, (unsigned)Function);
            }
        }

        g_ahci_controller_count++;

        Arch::RestoreInterrupts(_ahci_rflags);
    }

    BOOL RerouteControllerInterrupt(U8 Index, U8 apicId){
        if((int)Index >= g_ahci_controller_count) return FALSE;
        AHCIDriver &DRV = g_ahci_controllers[Index];

        if(DRV.using_msi){
            if(DRV.msi_cap_offset == 0) return FALSE;
            // Program MSI address to target APIC ID
            return MSI::SetMSIDestination(DRV.bus, DRV.dev, DRV.func, DRV.msi_cap_offset, apicId);
        } else {
            // Legacy IRQ via IOAPIC: DRV.legacy_irq holds IRQ (GSI)
            if(DRV.legacy_irq == 0) return FALSE;
            // Vector is already stored in DRV.IntVector (0x20 + irq)
            ACPI::IOAPIC::IOApicRedirectToCPU(DRV.legacy_irq, DRV.IntVector, IOAPIC_FLAGS_DEFAULT, apicId);
            return TRUE;
        }
    }

    BOOL MoveControllerInterruptToCPU(U8 Index, U8 apicId){
        if((int)Index >= g_ahci_controller_count) return FALSE;
        AHCIDriver &DRV = g_ahci_controllers[Index];

        // Save device IE/GHC state and clear them so device won't fire during change
        DRV.saved_ghc = DRV.regs->ghc;
        for(int p = 0; p < 32; ++p){
            DRV.saved_port_ie[p] = DRV.regs->ports[p].ie;
            DRV.regs->ports[p].ie = 0;
        }
        // global
        DRV.regs->ghc &= ~(1 << 1);

        // Save routing details and program new destination
        if(DRV.using_msi){
            if(DRV.msi_cap_offset == 0) goto restore_and_fail;
            // Read current MSI message address low dword (cap offset+4)
            DRV.saved_msi_addr_lo = PCI::ReadDword(DRV.bus, DRV.dev, DRV.func, DRV.msi_cap_offset + 0x04);
            // If 64-bit, read upper dword as well (Message Address upper)
            U32 reg0 = PCI::ReadDword(DRV.bus, DRV.dev, DRV.func, DRV.msi_cap_offset);
            U16 mc = (U16)(reg0 >> 16);
            if (mc & (1u << 7)) {
                DRV.saved_msi_addr_hi = PCI::ReadDword(DRV.bus, DRV.dev, DRV.func, DRV.msi_cap_offset + 0x08);
            } else {
                DRV.saved_msi_addr_hi = 0;
            }

            if(!MSI::SetMSIDestination(DRV.bus, DRV.dev, DRV.func, DRV.msi_cap_offset, apicId)) goto restore_and_fail;
        } else {
            if(DRV.legacy_irq == 0) goto restore_and_fail;
            // Save IOAPIC redirection high dword
            U8 regOff = REDTBL_ENTRY_FOR_GSI(DRV.legacy_irq) + 1;
            DRV.saved_ioapic_redt_high = ACPI::IOAPIC::IOAPICRead(regOff);
            // Redirect to chosen APIC
            ACPI::IOAPIC::IOApicRedirectToCPU(DRV.legacy_irq, DRV.IntVector, IOAPIC_FLAGS_DEFAULT, apicId);
        }

        DRV.saved_valid = TRUE;
        // Re-enable device interrupt enables
        DRV.regs->ghc = DRV.saved_ghc;
        for(int p = 0; p < 32; ++p){
            DRV.regs->ports[p].ie = DRV.saved_port_ie[p];
        }

        return TRUE;

    restore_and_fail:
        // restore what we cleared
        DRV.regs->ghc = DRV.saved_ghc;
        for(int p = 0; p < 32; ++p){
            DRV.regs->ports[p].ie = DRV.saved_port_ie[p];
        }
        return FALSE;
    }

    BOOL RestoreControllerInterrupt(U8 Index){
        if((int)Index >= g_ahci_controller_count) return FALSE;
        AHCIDriver &DRV = g_ahci_controllers[Index];
        if(!DRV.saved_valid) return FALSE;

        // Disable device interrupts while we restore
        UNUSED__ U32 cur_ghc = DRV.regs->ghc;
        DRV.regs->ghc &= ~(1 << 1);
        for(int p = 0; p < 32; ++p){
            DRV.regs->ports[p].ie = 0;
        }

        if(DRV.using_msi){
            if(DRV.msi_cap_offset != 0){
                // Restore message address low and possibly high
                PCI::WriteDword(DRV.bus, DRV.dev, DRV.func, DRV.msi_cap_offset + 0x04, DRV.saved_msi_addr_lo);
                U32 reg0 = PCI::ReadDword(DRV.bus, DRV.dev, DRV.func, DRV.msi_cap_offset);
                U16 mc = (U16)(reg0 >> 16);
                if (mc & (1u << 7)) {
                    PCI::WriteDword(DRV.bus, DRV.dev, DRV.func, DRV.msi_cap_offset + 0x08, DRV.saved_msi_addr_hi);
                }
            }
        } else {
            // Restore IOAPIC redirection high dword
            if(DRV.legacy_irq != 0){
                U8 regOff = REDTBL_ENTRY_FOR_GSI(DRV.legacy_irq) + 1;
                // Re-write low dword to keep vector/flags, then restore high
                U32 low = ACPI::IOAPIC::IOAPICRead(regOff - 1);
                ACPI::IOAPIC::IOAPICWrite(regOff - 1, low);
                ACPI::IOAPIC::IOAPICWrite(regOff, DRV.saved_ioapic_redt_high);
            }
        }

        // restore device IE state
        DRV.regs->ghc = DRV.saved_ghc;
        for(int p = 0; p < 32; ++p){
            DRV.regs->ports[p].ie = DRV.saved_port_ie[p];
        }

        DRV.saved_valid = FALSE;
        return TRUE;
    }

    VOID InitializeAllControllers() {

        for(int i = 0; i < g_ahci_controller_count; i++) {
            AHCIDriver &DRV = g_ahci_controllers[i];

            // PASTIKAN SAKLAR GLOBAL MATI DULU
            DRV.regs->ghc &= ~(1 << 1); // GHC.IE = 0

            U32 PortImplemented = DRV.regs->pi;

            /*Printk::Write(Printk::Level::LOG_DEBUG, " Controller %d at %02X:%02X:%02X - Ports Implemented: 0x%08X\n",
                i, (unsigned)DRV.bus, (unsigned)DRV.dev, (unsigned)DRV.func,
                (unsigned)PortImplemented);*/

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

                            //Printk::Write(Printk::Level::LOG_INFO, " Controller %d Port %d initialized as SATA drive\n", i, portnum);

                            IBlockDevice *NewDisk = new AHCIBlockDevice(&DRV, (U8)portnum);

                            if(DeviceManager::RegisterBlockDevice(NewDisk)){
                                Printk::Write(Printk::Level::LOG_DEBUG, " AHCI: Registered block device for Controller %d Port %d\n", i, portnum);
                            } else {
                                Printk::Write(Printk::Level::LOG_ERR, " AHCI: Failed to register block device for Controller %d Port %d\n", i, portnum);
                                delete NewDisk;
                            }
                        }
                        DRV.port_device[portnum] = DevType;
                    } else {
                        Printk::Write(Printk::Level::LOG_ERR, " Controller %d Port %d failed to initialize\n", i, portnum);
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

    // Ambil AHCI controller berdasarkan index dan kembalikan sebagai struct (by value)
    // Jika index tidak valid, kembalikan struct default dengan regs=nullptr dan initialized=false
    AHCIDriver GetController(int index) {
        AHCIDriver ret{};
        ret.regs = nullptr;
        ret.initialized = false;
        ret.IntVector = 0;
        ret.using_msi = false;
        ret.msi_cap_offset = 0;
        ret.legacy_irq = 0;
        ret.saved_valid = false;
        // Jika valid, salin data dari tabel global
        if (index >= 0 && index < g_ahci_controller_count) {
            ret = g_ahci_controllers[index];
        }
        return ret;
    }

    // Mengembalikan nomor port aktif pertama (SATA) atau -1 jika tidak ada.
    VAL32 FindActivePortNum(const AHCIDriver &Driver) {
        // Jika register belum dipetakan atau belum diinisialisasi, aman kembalikan -1
        if (!Driver.regs || !Driver.initialized) {
            return (VAL32)-1;
        }

        // Port yang diimplementasikan ditandai pada bitfield PI
        U32 implemented = Driver.regs->pi;
        for (VAL32 port = 0; port < 32; ++port) {
            if ((implemented & (1u << port)) == 0)
                continue; // port tidak diimplementasikan

            if (Driver.port_device[port] == DeviceType::SATA) {
                return port;
            }
        }
        return (VAL32)-1;
    }

    AHCIPortInfo GetPortInfo(int ConIndex){
        AHCIPortInfo TheInfo{};
        TheInfo.AhciDRV = GetController(ConIndex);
        TheInfo.controller_index = 0;
        TheInfo.port_number = FindActivePortNum(TheInfo.AhciDRV);
        return TheInfo;
    }
}