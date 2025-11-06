#include "pci.hpp"
#include <rossys.hpp>
#include <rosval.h>
#include <logging.hpp>
#include "../pic/pic.hpp"
#include "../../intidt/idt.hpp"

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

    // Attempt to enable legacy INTx (PCI interrupt line) for the given device
    // and register the provided IRQ handler. Returns IRQ number on success,
    // 0 on failure.
    U8 EnableLegacyINTxForDevice(U8 Bus, U8 Device, U8 Function, void (*irq_handler)()){
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
        U8 irq = (U8)(ReadDword(Bus, Device, Function, 0x3C) & 0xFF);
        if (irq == 0xFF || irq == 0x00) {
            Printk::Write(Printk::Level::LOG_WARNING, " PCI: Invalid IRQ 0x%02x for %02x:%02x.%u\n", (unsigned)irq, (unsigned)Bus, (unsigned)Device, (unsigned)Function);
            return 0;
        }

        // Unmask IRQ in PIC if legacy IRQ (<16). For IOAPIC/GSI >=16, more setup is required.
        if (irq < 16) {
            PIC::EnableIRQ(irq);
        } else {
            Printk::Write(Printk::Level::LOG_WARNING, " PCI: IRQ %u >=16 - IOAPIC routing may be required (not implemented here)\n", (unsigned)irq);
        }

        // Register handler on vector (PIC offset base is 0x20 -> vector = 0x20 + irq)
        U8 vector = (U8)(0x20 + irq);
        IDT::RegisterInterruptHandler(vector, irq_handler);
        Printk::Write(Printk::Level::LOG_INFO, " PCI: Registered legacy IRQ %u -> vector 0x%02x for %02x:%02x.%u\n", (unsigned)irq, (unsigned)vector, (unsigned)Bus, (unsigned)Device, (unsigned)Function);

        return irq;
    }
}