#include "pic.hpp"
#include <rosval.h>
#include <port.hpp>
#include <logging.hpp>

namespace PIC {
    using namespace Port;
    void InitializePIC() {
        // Remap PIC to avoid conflicts with CPU exceptions
        Remap(0x20, 0x28);

        // Mask all IRQs initially
        Outb(0x21, 0xFF); // Master PIC data port
        Outb(0xA1, 0xFF); // Slave PIC data port

        EnableIRQ(1); // Keyboard IRQ
        EnableIRQ(0); // Timer IRQ
        Printk::Write(Printk::Level::LOG_INFO, "[PIC] PIC Initialized\n");
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

        Printk::Write(Printk::Level::LOG_INFO, "[PIC] Remapped PIC with offsets 0x%02x and 0x%02x\n", offset1, offset2);
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

        Printk::Write(Printk::Level::LOG_INFO, "[PIC] Enabled & Unmasked IRQ %u\n", (unsigned)irq);
    }

    void SendEOI(U8 irq) {
        if (irq >= 8) {
            Outb(0xA0, 0x20); // Send to Slave PIC
        }
        Outb(0x20, 0x20);     // Send to Master PIC
    }
}