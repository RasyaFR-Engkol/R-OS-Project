#pragma once

#include "../kernel/driver/pic/pic.hpp"
#include "rostime.hpp"
#include "rosval.h"

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

        static inline VOID Interrupt(U64 Vector) {
            asm volatile (
                "int %0"
                :
                : "N"(Vector)
                : "memory", "cc"
            );
        }

        static inline U64 RdTSC(){
            U32 lo, hi;
            asm volatile ("rdtsc" : "=a"(lo), "=d"(hi));
            return ((U64)hi << 32) | (U64)lo;
        }
    }

        // Model-specific register helpers (RDMSR/WRMSR) and convenient EFER/STAR accessors.
        namespace MSR {
            static inline U64 Read(U32 msr) {
                U32 lo, hi;
                asm volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
                return ((U64)hi << 32) | (U64)lo;
            }

            static inline void Write(U32 msr, U64 value) {
                U32 lo = (U32)value;
                U32 hi = (U32)(value >> 32);
                asm volatile ("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
            }

            // Common MSR numbers (IA32 family)
            constexpr U32 IA32_APIC_BASE = 0x0000001Bu;
            constexpr U32 IA32_EFER = 0xC0000080u;
            constexpr U32 IA32_STAR = 0xC0000081u;
            constexpr U32 IA32_LSTAR = 0xC0000082u;
            constexpr U32 IA32_FMASK = 0xC0000084u;

            static inline U64 ReadEFER() { return Read(IA32_EFER); }
            static inline void WriteEFER(U64 v) { Write(IA32_EFER, v); }

            static inline U64 ReadSTAR() { return Read(IA32_STAR); }
            static inline void WriteSTAR(U64 v) { Write(IA32_STAR, v); }
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

// Time helpers (implemented in kernel/rostime.cpp).
// Declarations are in Include/rostime.hpp which is included above.
namespace Arch {
    namespace Power{
        VOID Shutdown();
        VOID Reboot();
        VOID Sleep();
    }
}

