#pragma once

#include "rosval.h"

namespace Arch {
    namespace Time {
        // Return current tick counter. Prefer LAPIC ticks when available.
        U64 NowTicks();

        // Estimate current tick frequency in Hz (uses LAPIC when available).
        U32 TickHz();

        // Sleep for a number of ticks (uses NowTicks())
        void SleepTicks(U64 ticks);

        // Sleep for approximately ms milliseconds.
        void SleepMs(U64 ms);

        // Convenience aliases
        void Sleep(U64 ms);
        void SleepSeconds(U64 s);

        // Whether LAPIC timing is active (LapicHz != 0)
        bool LapicTimingActive();
    }
}
