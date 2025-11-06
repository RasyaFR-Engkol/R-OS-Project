#include <rossys.hpp>
#include <rosval.h>
#define PRINTK_MODULE_NAME "MSI"
#include "msixmsi.hpp"
#include <logging.hpp>

// Ini adalah "register" di dalam CPU kamu (Local APIC)
// yang menerima interrupt. Alamat ini standar.
constexpr U32 MSI_MSG_ADDRESS = 0xFEE00000;

/* module name provided via PRINTK_MODULE_NAME */

namespace MSI{

    U8 EnableMSI(U8 bus, U8 dev, U8 func, U8 msi_cap_offset, void (*handler)()){
        U8 Vector = IDT::AllocateVector();
        if(Vector == 0){
            Printk::Write(Printk::Level::LOG_ERR, " Failed to allocate interrupt vector for MSI\n");
            return 0;
        }

        Printk::Write(Printk::Level::LOG_INFO, " Allocated vector 0x%02x for MSI\n", (unsigned)Vector);
        
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
}