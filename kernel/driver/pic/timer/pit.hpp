#pragma once

#include <rosval.h>
#include <port.hpp>

namespace PIT {
    extern U16 PITReload;
    extern volatile U64 ticks;
    extern volatile U64 missed_ticks;
    // Initialize PIT to the requested frequency in Hz (typical values: 18, 100, 1000)
    void InitializePIT(U32 hz);

    static inline U16 ReadPITCounter() {
        using namespace Port;
        // Latch the current count value
        Outb(0x43, 0x00); // Channel 0, latch command
        U8 low = Inb(0x40);
        U8 high = Inb(0x40);
        return (static_cast<U16>(high) << 8) | low;
    }
}
