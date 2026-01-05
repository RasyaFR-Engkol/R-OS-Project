#include <rossys.hpp>
#include <rosval.h>
#define PRINTK_MODULE_NAME "MSI"
#include "msixmsi.hpp"
#include <logging.hpp>
#include <mm.hpp>

// Ini adalah "register" di dalam CPU kamu (Local APIC)
// yang menerima interrupt. Alamat ini standar.
constexpr U32 MSI_MSG_ADDRESS = 0xFEE00000;

struct MSIXTableEntry {
    volatile U32 MsgAddrLo;
    volatile U32 MsgAddrHi;
    volatile U32 MsgData;
    volatile U32 VectorControl;
};

/* module name provided via PRINTK_MODULE_NAME */

namespace MSI{

    U8 EnableMSI(U8 bus, U8 dev, U8 func, U8 msi_cap_offset, void (*handler)(void *context)){
        U8 Vector = IDT::AllocateVector();
        if(Vector == 0){
            Printk::Write(Printk::Level::LOG_ERR, " Failed to allocate interrupt vector for MSI\n");
            return 0;
        }
        
        // MSI control and layout: first dword at capability offset contains
        // 8-bit CapID, 8-bit NextPtr, 16-bit Message Control (MC)
        U32 reg0 = PCI::ReadDword(bus, dev, func, msi_cap_offset);
        U16 mc = (U16)(reg0 >> 16);
        bool is64 = (mc & (1u << 7)) != 0; // 64-bit capable?

        // Program Message Address (lower 32 bits) to Local APIC (xAPIC) base
        U32 msg_addr_lo = MSI_MSG_ADDRESS; // dest APIC ID 0 (bits 19:12)
        // Program Message Data: vector in bits[7:0], Fixed delivery (000), edge (bit14=0), deassert (bit15=0)
        U32 msg_data = (U32)(Vector & 0xFF);

        // Write Address and Data based on 32/64-bit capability
        PCI::WriteDword(bus, dev, func, msi_cap_offset + 0x04, msg_addr_lo);
        if (is64) {
            // Upper 32 bits of address (we use 0 for xAPIC 32-bit address)
            PCI::WriteDword(bus, dev, func, msi_cap_offset + 0x08, 0);
            // Message Data at +0x0C
            PCI::WriteDword(bus, dev, func, msi_cap_offset + 0x0C, msg_data);
        } else {
            // 32-bit address: Message Data at +0x08
            PCI::WriteDword(bus, dev, func, msi_cap_offset + 0x08, msg_data);
        }

        // Register ISR for the allocated vector
        IDT::RegisterInterruptHandler(Vector, handler);

        // Enable MSI (bit 0 in Message Control)
        mc |= 1u;
        U32 reg0_new = (reg0 & 0x0000FFFFu) | ((U32)mc << 16);
        PCI::WriteDword(bus, dev, func, msi_cap_offset, reg0_new);

        return Vector;
    }

        BOOL SetMSIDestination(U8 bus, U8 dev, U8 func, U8 msi_cap_offset, U8 apic_id){
            if(msi_cap_offset == 0) return FALSE;

            U32 reg0 = PCI::ReadDword(bus, dev, func, msi_cap_offset);
            U16 mc = (U16)(reg0 >> 16);
            bool is64 = (mc & (1u << 7)) != 0; // 64-bit capable?

            // xAPIC message address base
            U32 msg_addr_lo = MSI_MSG_ADDRESS | ((U32)apic_id << 12);

            // Write Address and Data based on 32/64-bit capability
            PCI::WriteDword(bus, dev, func, msi_cap_offset + 0x04, msg_addr_lo);
            if (is64) {
                // Upper 32 bits of address (we use 0 for xAPIC 32-bit address)
                PCI::WriteDword(bus, dev, func, msi_cap_offset + 0x08, 0);
                // Message Data at +0x0C remains unchanged (do not touch vector)
            } else {
                // 32-bit address: Message Data at +0x08 remains unchanged
            }

            Printk::Write(Printk::Level::LOG_INFO, " MSI destination for %02x:%02x:%02x set to APIC ID %u\n",
                (unsigned)bus, (unsigned)dev, (unsigned)func, (unsigned)apic_id);

            return TRUE;
        }

    U8 EnableMSIX(U8 bus, U8 dev, U8 func, U8 msix_cap_offset, void (*handler)(void *context)) {

        // 1. Baca Message Control (Offset + 2)
        // Struktur: [15: Enable] [14: Func Mask] [13:11 Rsvd] [10:0 Table Size N-1]
        UNUSED__ U32 cap_reg_val = PCI::ReadDword(bus, dev, func, msix_cap_offset);
        U16 msg_ctrl = PCI::ReadWord(bus, dev, func, msix_cap_offset + 2);
        U16 table_size = (msg_ctrl & 0x7FF) + 1;
        
        U32 table_info = PCI::ReadDword(bus, dev, func, msix_cap_offset + 4);
        U8  bir = (U8)(table_info & 0x7); 
        U32 table_offset = table_info & ~0x7;

        U32 bar_reg = 0x10 + (bir * 4);
        U32 bar_lo = PCI::ReadDword(bus, dev, func, bar_reg);
        U64 bar_phys = 0;
        
        if ((bar_lo & 0x6) == 0x4) {
            U32 bar_hi = PCI::ReadDword(bus, dev, func, bar_reg + 4);
            bar_phys = ((U64)bar_hi << 32) | (bar_lo & ~0xF);
        } else {
            bar_phys = (bar_lo & ~0xF);
        }

        if (bar_phys == 0) return 0;
        
        U64 msix_table_phys_start = bar_phys + table_offset;

        // ===============================================
        // FIX: MAPPING MANUAL (JANGAN PAKE HHDM)
        // ===============================================
        
        // 1. Hitung Alignment Page (4KiB)
        UPTR PagePhysStart = msix_table_phys_start & ~(PAGE_SIZE - 1);
        UPTR PageOffset    = msix_table_phys_start & (PAGE_SIZE - 1);
        
        // 2. Hitung berapa page yang perlu di-map
        // Satu entry MSI-X itu 16 bytes.
        U32 table_bytes = table_size * 16;
        // Kalau tabelnya nyebwrang page boundary, kita butuh map lebih dari 1 page
        U32 pages_needed = (PageOffset + table_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

        // 3. Alokasi Virtual Address Baru
        void* VirtAddr = PageAlloc::VirtualAllocPages(pages_needed);
        if (!VirtAddr) {
            Printk::Write(Printk::Level::LOG_ERR, " [MSI-X] Failed to allocate virtual pages!\n");
            return 0;
        }

        // 4. Map Physical ke Virtual
        // PENTING: Pake flag PAGE_PCD (Page Cache Disable) karena ini MMIO!
        PFLAGS Flags = PAGE_PRESENT | PAGE_RW | PAGE_PCD; 
        if (!PageAlloc::MapPages(KernelPML4, PagePhysStart, (UPTR)VirtAddr, pages_needed, Flags)) {
             Printk::Write(Printk::Level::LOG_ERR, " [MSI-X] Failed to map MMIO pages!\n");
             return 0;
        }
        
        // 5. Hitung pointer final
        volatile MSIXTableEntry* msix_table_virt = (volatile MSIXTableEntry*)((UPTR)VirtAddr + PageOffset);

        // ===============================================
        // SISANYA SAMA SEPERTI SEBELUMNYA
        // ===============================================

        // 5. Alokasi Vector IDT
        U8 vector = IDT::AllocateVector();
        if (vector == 0) return 0;

        // 6. Setup Entry 0 (Unmasked)
        msix_table_virt[0].MsgAddrLo = MSI_MSG_ADDRESS; 
        msix_table_virt[0].MsgAddrHi = 0;
        msix_table_virt[0].MsgData = vector;
        msix_table_virt[0].VectorControl = 0; // Unmask
        
        IDT::RegisterInterruptHandler(vector, handler);

        // 7. ENABLE GLOBAL MSI-X
        msg_ctrl |= (1 << 15); 
        msg_ctrl &= ~(1 << 14); 
        PCI::WriteWord(bus, dev, func, msix_cap_offset + 2, msg_ctrl);

        // 8. ENABLE BUS MASTER & DISABLE LEGACY
        U16 cmd = PCI::ReadWord(bus, dev, func, 0x04);
        cmd |= (1 << 2); 
        cmd |= (1 << 10);
        PCI::WriteWord(bus, dev, func, 0x04, cmd);

        return vector;
    }
}