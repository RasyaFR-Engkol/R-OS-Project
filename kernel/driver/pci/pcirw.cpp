#include "pci.hpp"
#include <rossys.hpp>
#include <rosval.h>
#include <logging.hpp>
#include "../pic/pic.hpp"
#include "../../intidt/idt.hpp"
#include <firmware/acpi.hpp>
#include <../firmware/chipset/chipset.hpp>

namespace PCI{
    using namespace Printk;

    U32 ReadDword(U8 Bus, U8 Device, U8 Function, U8 Offset){
        U32 LBus = (U32)Bus;
        U32 LDevice = (U32)Device;
        U32 LFunc = (U32)Function;
        U32 Address = (U32)((LBus << 16) | (LDevice << 11) | (LFunc << 8) | (Offset & 0xFC) | 0x80000000);

        // Tulis alamat ke port config address
        Port::Outl(0xCF8, Address);

        // Baca hasilnya
        return Port::Inl(0xCFC);
    }

        void WriteDword(U8 Bus, U8 Device, U8 Function, U8 Offset, U32 Value){
            U32 LBus = (U32)Bus;
            U32 LDevice = (U32)Device;
            U32 LFunc = (U32)Function;
            U32 Address = (U32)((LBus << 16) | (LDevice << 11) | (LFunc << 8) | (Offset & 0xFC) | 0x80000000);

            // Write address to config address port
            Port::Outl(0xCF8, Address);
            // Write the value to config data port
            Port::Outl(0xCFC, Value);
        }

    U16 ReadWord(U8 Bus, U8 Device, U8 Function, U8 Offset){
        // 1. Setup Alamat (Harus aligned 32-bit, jadi & 0xFC)
        U32 Address = (U32)((Bus << 16) | (Device << 11) | (Function << 8) | (Offset & 0xFC) | 0x80000000);
        Port::Outl(0xCF8, Address);

        // 2. Trik Geser Port Data
        // Kalau offset misal 0x02, kita baca dari port 0xCFC + 2 = 0xCFE
        return Port::Inw(0xCFC + (Offset & 2));
    }

    U8 ReadByte(U8 Bus, U8 Device, U8 Function, U8 Offset){
        U32 Address = (U32)((Bus << 16) | (Device << 11) | (Function << 8) | (Offset & 0xFC) | 0x80000000);
        Port::Outl(0xCF8, Address);

        // Kalau offset misal 0x01, kita baca dari port 0xCFC + 1 = 0xCFD
        return Port::Inb(0xCFC + (Offset & 3));
    }

    void WriteWord(U8 Bus, U8 Device, U8 Function, U8 Offset, U16 Value){
        U32 Address = (U32)((Bus << 16) | (Device << 11) | (Function << 8) | (Offset & 0xFC) | 0x80000000);
        Port::Outl(0xCF8, Address);

        // Tulis 16-bit ke port offset yang sesuai
        Port::Outw(0xCFC + (Offset & 2), Value);
    }

    void WriteByte(U8 Bus, U8 Device, U8 Function, U8 Offset, U8 Value){
        U32 Address = (U32)((Bus << 16) | (Device << 11) | (Function << 8) | (Offset & 0xFC) | 0x80000000);
        Port::Outl(0xCF8, Address);

        // Tulis 8-bit ke port offset yang sesuai
        Port::Outb(0xCFC + (Offset & 3), Value);
    }

    // Attempt to enable legacy INTx (PCI interrupt line) for the given device
    // and register the provided IRQ handler. Returns IRQ number on success,
    // 0 on failure.
    U8 EnableLegacyINTxForDevice(U8 Bus, U8 Device, U8 Function, void (*irq_handler)(void *context)){
        // Read status to check capabilities
        U32 reg4 = ReadDword(Bus, Device, Function, 0x04);
        U16 status = (U16)((reg4 >> 16) & 0xFFFF);

        // If device has MSI and it's enabled, try to disable MSI so INTx works
        if (status & (1 << 4)) {
            U8 cap = (U8)(ReadDword(Bus, Device, Function, 0x34) & 0xFF);
            U8 ptr = cap;
            while (ptr) {
                U32 capval = ReadDword(Bus, Device, Function, ptr);
                U8 id = (U8)(capval & 0xFF);
                U8 next = (U8)((capval >> 8) & 0xFF);
                if (id == 0x05) { // MSI
                    U16 msgctl = (U16)(capval >> 16);
                    if (msgctl & 0x1) {
                        // clear MSI enable
                        msgctl &= ~0x1u;
                        U32 newcap = (capval & 0x0000FFFFu) | ((U32)msgctl << 16);
                        WriteDword(Bus, Device, Function, ptr, newcap);
                        Printk::Write(Printk::Level::LOG_INFO, " PCI: Disabled MSI for %02x:%02x.%u\n", (unsigned)Bus, (unsigned)Device, (unsigned)Function);
                    }
                    break;
                }
                ptr = next;
            }
        }

        // Ensure INTx isn't disabled in command register (bit 10 = INTx disable)
        U16 cmd = (U16)(reg4 & 0xFFFF);
        if (cmd & (1 << 10)) {
            cmd &= ~(1 << 10);
            U32 newcmd = (reg4 & 0xFFFF0000u) | cmd;
            WriteDword(Bus, Device, Function, 0x04, newcmd);
            Printk::Write(Printk::Level::LOG_INFO, " PCI: Cleared INTx disable for %02x:%02x.%u\n", (unsigned)Bus, (unsigned)Device, (unsigned)Function);
        }

        // Read assigned legacy IRQ (Interrupt Line at offset 0x3C)
        U8 irq_line = (U8)(ReadDword(Bus, Device, Function, 0x3C) & 0xFF);
        U8 irq_pin  = (U8)((ReadDword(Bus, Device, Function, 0x3C) >> 8) & 0xFF); // Ambil PIN (1-4)
        
        U8 final_gsi = irq_line; 

        using namespace Firmware::Chipset;

        // Cek Chipset Global yang udah dideteksi pas boot
        if (g_DetectedChipset == CHIPSET_ICH9) {
            // Logika ICH9: Swizzling
            // Rumus simple VBox: GSI = 16 + ((Slot + Pin - 1) % 4)
            // Validasi Pin dulu (harus 1-4, INTA-INTD)
            if (irq_pin >= 1 && irq_pin <= 4) {
                final_gsi = 16 + ((Device + (irq_pin - 1)) % 4);
                
                // Setup IOAPIC buat GSI baru ini
                Printk::Write(Printk::Level::LOG_INFO, "[PCI] ICH9 Remap Slot %d Pin %d -> GSI %d\n", Device, irq_pin, final_gsi);
                
                // PENTING: Level Triggered + Active Low buat PCI
                ACPI::IOAPIC::IOApicRedirect(final_gsi, 0x20 + final_gsi, IOAPIC_FLAGS_LEVEL | IOAPIC_FLAGS_LOW);
                IDT::RegisterInterruptHandler(0x20 + final_gsi, irq_handler);
                
                return final_gsi; // Return GSI baru
            }
        }

        // Logika PIIX3 / Fallback (Pake nilai 0x3C mentah-mentah)
        if (irq_line < 16) {
            PIC::EnableIRQ(irq_line);
            // Default flags (Edge/High) biasanya cukup buat legacy mode via IOAPIC override
            ACPI::IOAPIC::IOApicRedirect(irq_line, 0x20 + irq_line, IOAPIC_FLAGS_DEFAULT);
            IDT::RegisterInterruptHandler(0x20 + irq_line, irq_handler);
        }
        
        return irq_line;
        }
}