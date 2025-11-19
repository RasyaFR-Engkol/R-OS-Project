#pragma once

#include "../../madt/madt.hpp"

namespace ACPI {
    namespace Timer {
        // Lapic tick counter (incremented by the LAPIC timer IRQ handler)
        extern volatile U64 LapicTicks;
        extern U32 LapicHz;
        extern volatile U64 ticks;

        // Initialize the LAPIC timer to a target frequency in Hz.
        // Vector is the interrupt vector to deliver (typically 0x20 for
        // legacy IRQ0 replacement). desiredHz is the target tick frequency
        // (e.g., 100). If periodic is TRUE the timer will be set to periodic mode.
        VOID InitializeLapicTimer(U8 Vector, U32 desiredHz, BOOL periodic);
    }
}
