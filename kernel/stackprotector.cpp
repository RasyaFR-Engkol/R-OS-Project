// Minimal implementations for stack protector symbols when building a freestanding kernel
// Provide __stack_chk_guard and __stack_chk_fail so -fstack-protector-strong links

#include "log/printk/printk.hpp"
#include "rosval.h"
ABI_C {
    // A non-zero guard value; kernel can randomize this at boot if desired.
    unsigned long __stack_chk_guard = 0xDEADBEEFBADC0FFEull;

    // Minimal direct port I/O helpers (no dependencies)
    static inline void outb(unsigned short port, unsigned char val) {
        asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
    }
    static inline unsigned char inb(unsigned short port) {
        unsigned char ret;
        asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
        return ret;
    }

    // Write a single char to COM1 (0x3F8) with a small timeout to avoid
    // blocking forever if serial is unavailable. This is intentionally
    // standalone and does not call other kernel subsystems.
    static void serial_putc_direct(char c) {
        const unsigned short port = 0x3F8;
        unsigned int t = 100000u;
        while (!(inb((unsigned short)(port + 5)) & 0x20)) {
            if (--t == 0) return;
        }
        outb(port, (unsigned char)c);
    }

    static void serial_write_direct(const char *s) {
        if (!s) return;
        while (*s) { serial_putc_direct(*s++); }
    }

    static void print_hex_nibble(unsigned int v) {
        char c = (char)(v < 10 ? ('0' + v) : ('a' + (v - 10)));
        serial_putc_direct(c);
    }

    static void serial_write_hex64(unsigned long long v) {
        // print without 0x prefix: 16 hex digits
        for (int i = (16 - 1); i >= 0; --i) {
            unsigned int nib = (unsigned int)((v >> (i * 4)) & 0xFULL);
            print_hex_nibble(nib);
        }
    }

    // Called when stack smashing is detected. Try to emit a tiny diagnostic
    // (via direct serial port I/O) then halt the CPU. Keep implementation
    // minimal to avoid pulling in other subsystems that may be compromised.
    NORET void __stack_chk_fail(void) {
        // Disable interrupts
        asm volatile("cli");

        // Attempt to print a few registers to help locate the culprit.
        unsigned long long rbp = 0;
        unsigned long long rip = 0;
        // Read RBP
        asm volatile ("mov %%rbp, %0" : "=r"(rbp));
        // __builtin_return_address(0) should give the caller's return address
        rip = (unsigned long long)__builtin_return_address(0);

        Printk::Write(Printk::Level::LOG_EMERG, "\n\n\n\n\n\n\nStack smashing detected!\n" 
            "Kernel stack broken\n\n\n"
            "First deteccted Stack Smash RIP: %p\n"
            "RBP Related stack smashing: %p\n" 
            "\n\n System halted (SYSBRK)\n\n", (void*)rip, (void*)rbp);

        //serial_write_direct("*** STACK SMASH DETECTED ***\n");

        //serial_write_direct("RBP=0x"); serial_write_hex64(rbp); serial_write_direct("\n");
        //serial_write_direct("RIP=0x"); serial_write_hex64(rip); serial_write_direct("\n");

        // Optionally, dump a small region of stack (first 8 qwords) to help
        // offline analysis (may read invalid memory in exotic cases).
        serial_write_direct("Stack (top 8 qwords):\n");
        unsigned long long *sp = (unsigned long long*)rbp;
        for (int i = 0; i < 8; ++i) {
            unsigned long long val = 0;
            // Try to read memory; protect lightly by checking pointer value
            if ((unsigned long long)(sp + i) > 0x1000ULL) {
                val = *(sp + i);
            }
            serial_write_hex64(val);
            serial_write_direct("\n");
        }

        // Halt forever
        for (;;) {
            asm volatile("hlt");
        }
    }
}
