#pragma once

// PIT Driver
#include "../kernel/driver/pic/pic.hpp"
#include "../kernel/driver/pic/timer/pit.hpp"

// Semua linker di atas harusnya berakhir disini setelah
// keterangan ini
// 
// maka kita akan bikin namespace Arch

typedef U64 LOCKRFLAGS;
namespace Arch {
    namespace ASM {
        static inline void HaltCPU() {
            asm volatile ("hlt");
        }

        static inline void Sti() {
            asm volatile ("sti");
        }

        static inline void Cli() {
            asm volatile ("cli");
        }

        static inline void PauseCPU() {
            asm volatile ("pause");
        }

        static inline void CPURelax() {
            asm volatile ("rep nop");
        }

        static inline LOCKRFLAGS SaveAndDisballeInterrupts() {
            LOCKRFLAGS rflags;
            asm volatile (
                "pushfq\n"
                "popq %0\n"
                "cli"
                : "=r"(rflags)
                :
                : "memory"
            );
            return rflags;
        }

        static inline void RestoreInterrupts(LOCKRFLAGS rflags) {
            asm volatile (
                "pushq %0\n"
                "popfq"
                :
                : "r"(rflags)
                : "memory", "cc"
            );
        }

        static inline bool AreInterruptsEnabled() {
            LOCKRFLAGS rflags;
            asm volatile (
                "pushfq\n"
                "popq %0"
                : "=r"(rflags)
                :
                : "memory"
            );
            return (rflags & (1 << 9)) != 0;
        }
    }

    // Backward-compatible wrappers so existing callers (Arch::X) keep working.
    static inline void HaltCPU() { return ASM::HaltCPU(); }
    static inline void Sti() { return ASM::Sti(); }
    static inline void Cli() { return ASM::Cli(); }
    static inline void PauseCPU() { return ASM::PauseCPU(); }
    static inline void CPURelax() { return ASM::CPURelax(); }
    static inline LOCKRFLAGS SaveAndDisballeInterrupts() { return ASM::SaveAndDisballeInterrupts(); }
    // Correctly-spelled alias for clarity
    static inline LOCKRFLAGS SaveAndDisableInterrupts() { return ASM::SaveAndDisballeInterrupts(); }
    static inline void RestoreInterrupts(LOCKRFLAGS rflags) { return ASM::RestoreInterrupts(rflags); }
    static inline bool AreInterruptsEnabled() { return ASM::AreInterruptsEnabled(); }
}

// Time helpers built on top of PIT (ticks). Provides simple sleeping
// primitives used during early boot and non-preemptive waits.
namespace Arch {
    namespace Time {
        // Return current PIT ticks (incremented by PIT IRQ handler)
        static inline U64 NowTicks() { return PIT::ticks; }

        // Estimate PIT frequency in Hz from the reload divisor. Falls back to
        // 100 Hz if divisor is zero or invalid.
        static inline U32 TickHz() {
            U16 reload = PIT::PITReload;
            if (reload == 0) return 100;
            const U32 PIT_BASE = 1193182U;
            return (U32)(PIT_BASE / (U32)reload);
        }

        // Sleep for a number of PIT ticks. If interrupts are enabled we use
        // HLT to wait efficiently; otherwise we busy-loop with CPURelax.
        static inline void SleepTicks(U64 ticks) {
            U64 target = PIT::ticks + ticks;
            if (Arch::AreInterruptsEnabled()) {
                while (PIT::ticks < target) asm volatile ("hlt");
            } else {
                while (PIT::ticks < target) Arch::CPURelax();
            }
        }

        // Sleep for approximately ms milliseconds. Uses TickHz() to convert
        // milliseconds to PIT ticks (rounded up).
        static inline void SleepMs(U64 ms) {
            U32 hz = TickHz();
            if (hz == 0) hz = 100;
            U64 ticks = (ms * (U64)hz + 999) / 1000ULL; // ceil
            SleepTicks(ticks);
        }

        // Convenience alias
        static inline void Sleep(U64 ms) { SleepMs(ms); }
    }
}

