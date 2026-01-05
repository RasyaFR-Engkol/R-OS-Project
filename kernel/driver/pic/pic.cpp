#include "rossys.hpp"
#define PRINTK_MODULE_NAME "PIC"
#include "pic.hpp"
#include <rosval.h>
#include <port.hpp>
#include <logging.hpp>

/* module name provided via PRINTK_MODULE_NAME */

namespace PIC {
    using namespace Port;
    BOOL G_StillLegacyINTx = FALSE;
    void InitializePIC() {
        // Remap PIC to avoid conflicts with CPU exceptions
        Remap(0x20, 0x28);

        // Mask all IRQs initially
        Outb(0x21, 0xFF); // Master PIC data port
        Outb(0xA1, 0xFF); // Slave PIC data port

        G_StillLegacyINTx = TRUE;

        EnableIRQ(1); // Keyboard IRQ
        EnableIRQ(0); // Timer IRQ
        Printk::Write(Printk::Level::LOG_INFO, " PIC Initialized\n");
    }

    void Remap(U8 offset1, U8 offset2) {
        U8 a1, a2;

        // Save masks
        a1 = Inb(0x21);
        a2 = Inb(0xA1);

        // Start initialization sequence in cascade mode
        Outb(0x20, 0x11);
        Outb(0xA0, 0x11);

        // Set vector offsets
        Outb(0x21, offset1);
        Outb(0xA1, offset2);

        // Tell Master PIC that there is a slave PIC at IRQ2 (0000 0100)
        Outb(0x21, 0x04);
        // Tell Slave PIC its cascade identity (0000 0010)
        Outb(0xA1, 0x02);

        // Set PICs to 8086/88 (MCS-80/85) mode
        Outb(0x21, 0x01);
        Outb(0xA1, 0x01);

        // Restore saved masks
        Outb(0x21, a1);
        Outb(0xA1, a2);

        Printk::Write(Printk::Level::LOG_INFO, " Remapped PIC with offsets 0x%02x and 0x%02x\n", offset1, offset2);
    }

    void EnableIRQ(U8 irq) {
        U16 port;
        U8 value;

        if (irq < 8) {
            port = 0x21; // Master PIC
        } else {
            port = 0xA1; // Slave PIC
            irq -= 8;
        }
        value = Inb(port) & ~(1 << irq);
        Outb(port, value);

        Printk::Write(Printk::Level::LOG_INFO, " Enabled & Unmasked IRQ %u\n", (unsigned)irq);
    }

    void SendEOI(U8 irq) {
        if (irq >= 8) {
            Outb(0xA0, 0x20); // Send to Slave PIC
        }
        Outb(0x20, 0x20);     // Send to Master PIC
    }

    VOID DisableIRQWhileAndMaskOldPIC(){
        Arch::ASM::Cli();

        // Start initialization sequence (ICW1)
        Port::Outb(PIC1_COMMAND, 0x11);
        Port::Outb(PIC2_COMMAND, 0x11);

        Port::Outb(PIC1_DATA, 0xF8); // Master offset
        Port::Outb(PIC2_DATA, 0xF8); // Slave offset

        // Kirim ICW3 (Master/Slave)
        Port::Outb(PIC1_DATA, 0x04);
        Port::Outb(PIC2_DATA, 0x02);

        // Kirim ICW4 (Environment)
        Port::Outb(PIC1_DATA, 0x01);
        Port::Outb(PIC2_DATA, 0x01);

        // Mask SEMUA interrupt di kedua PIC
        Port::Outb(PIC1_DATA, 0xFF);
        Port::Outb(PIC2_DATA, 0xFF);
        
        Printk::Write(Printk::Level::LOG_INFO, "Legacy PIC 8259 disabled.\n");

        G_StillLegacyINTx = FALSE;
    }
}