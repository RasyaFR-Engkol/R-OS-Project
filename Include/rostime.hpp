#pragma once

#include "rossys.hpp"
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

        U64 GetTickCount();
        U64 GetSecCount();

        U32 RTCToEpoch(Arch::CMOS::RTCTime T);

        // Convert milliseconds to ticks based on current TickHz()
        static inline U64 MsToTicks(U64 ms) {
                U32 hz = TickHz();
                if (hz == 0) hz = 100;
                return (ms * (U64)hz + 999) / 1000ULL; // ceil
        }

        // convert detik ke milidetik
        static inline U64 SecToMs(U64 s) {
            return s * 1000;
        }
    }
}
