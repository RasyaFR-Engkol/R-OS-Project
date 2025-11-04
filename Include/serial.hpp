#pragma once

#include "rosval.h"
#include <port.hpp>

namespace Serial{
    void Init();
    void Write(const char *s);
    //char Read();
    void Printf(const char *fmt, ...);
    // Non-blocking: attempts to read one char from COM1; returns TRUE if a char was read
    BOOL TryReadChar(char *out);
    // Poll incoming serial input and mirror to FBConsole and Serial (with basic line-editing)
    VOID PollToConsoles();
    // Enable IRQ-driven input (unmask IRQ4 and register ISR). Call after PIC/IDT init.
    VOID EnableIRQInput();
        static void SerialPutC(char c) {
        const U16 port = 0x3F8;
        // Wait for Transmitter Holding Register empty
        while (!(Port::Inb(port + 5) & 0x20)) {}
        Port::Outb(port, (U8)c);
    }

}