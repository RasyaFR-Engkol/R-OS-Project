#pragma once

#include "../../madt/madt.hpp"

#define IA32_TSC_DEADLINE_MSR 0x6E0


namespace ACPI {
    namespace Timer {
        // Lapic tick counter (incremented by the LAPIC timer IRQ handler)
        extern volatile U64 LapicTicks;
        extern U32 LapicHz;
        extern volatile U64 ticks;
        extern U64 RawApicHz;

        extern VOLATILE BOOL UsingTscDeadline;
        extern U64 TscTicksPerSystemTick; // Berapa cycle TSC untuk 10ms (100hz)

        extern U64 TSCFrequencyHz;
        extern U64 BootTSC;

        // Initialize the LAPIC timer to a target frequency in Hz.
        // Vector is the interrupt vector to deliver (typically 0x20 for
        // legacy IRQ0 replacement). desiredHz is the target tick frequency
        // (e.g., 100). If periodic is TRUE the timer will be set to periodic mode.
        VOID InitializeLapicTimer(U8 Vector, U32 desiredHz, BOOL periodic);

        // Helper untuk konversi waktu ke Raw Ticks
        U64 MicrosecondsToTicks(U64 us);
        U64 NanosecondsToTicks(U64 ns);
        U64 MillisecondsToTicks(U64 ms);
        
        // Ubah mode timer on-the-fly
        VOID SetOneShotMode(U64 ticks);
        VOID StopTimer();

        void SetTSCDeadline(U64 futureTsc);
        VOID Arm(U64 ticksFromNow);

        VOID GetTimeSinceBoot(U64 *RetSeconds, U64 *SubRetSeconds);
    }
}
