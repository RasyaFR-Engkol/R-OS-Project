#pragma once

// PIT Driver
#include "../kernel/driver/pic/pic.hpp"

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

