#pragma once

#include "../kernel/driver/pic/pic.hpp"
#include "port.hpp"
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

        static inline void Hti() {
            asm volatile (
                "sti\n"
                "hlt"
            );
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

        static inline VOID Mfence(){
            asm volatile ("mfence" ::: "memory");
        }

        static inline VOID Clflush(VOID* addr){
            asm volatile ("clflush (%0)" :: "r"(addr) : "memory");
        }

                STATIC INLINE U64 __cpuid(U32 info, U32 &eax, U32 &ebx, U32 &ecx, U32 &edx) {
            asm volatile (
                "cpuid"
                : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                : "a"(info)
            );

            return ((U64)edx << 32) | (U64)eax;
        }

        inline bool HasTSCDeadline() {
            uint32_t eax, ebx, ecx, edx;
            // CPUID Leaf 1, ECX Bit 24 = TSC-Deadline
            __cpuid(1, eax, ebx, ecx, edx); 
            return (ecx & (1 << 24));
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

    namespace CMOS {
        #define CMOS_ADDRESS 0x70
        #define CMOS_DATA    0x71

        enum {
            SECONDS = 0x00,
            MINUTES = 0x02,
            HOURS   = 0x04,
            DAY     = 0x07,
            MONTH   = 0x08,
            YEAR    = 0x09,
            STATUS_A = 0x0A,
            STATUS_B = 0x0B
        };

        STATIC INLINE INTN GetUpdateInProgFlags(){
            Port::Outb(CMOS_ADDRESS, STATUS_A);
            return (Port::Inb(CMOS_DATA) & 0x80);
        }

        STATIC INLINE U8 GetRegister(INTN Reg){
            Port::Outb(CMOS_ADDRESS, Reg);
            return (Port::Inb(CMOS_DATA));
        }

        STATIC U8 BCDToBin(U8 bcd){
            return ((bcd & 0xF0) >> 1) + ( (bcd & 0xF0) >> 3) + (bcd & 0x0f);
        }

        struct RTCTime {
            U8 second;
            U8 minute;
            U8 hour;
            U8 day;
            U8 month;
            U32 year;
        };

        STATIC INLINE RTCTime ReadRTC(){
            RTCTime time;
        
        // Tunggu sampai bit "Update In Progress" clear
        while (GetUpdateInProgFlags());

        time.second = GetRegister(SECONDS);
        time.minute = GetRegister(MINUTES);
        time.hour   = GetRegister(HOURS);
        time.day    = GetRegister(DAY);
        time.month  = GetRegister(MONTH);
        time.year   = GetRegister(YEAR);

        // Cek Status Register B untuk mengetahui format datanya
        uint8_t registerB = GetRegister(STATUS_B);

        // Jika bit 2 (DM) adalah 0, maka data dalam format BCD -> perlu convert
        if (!(registerB & 0x04)) {
            time.second = BCDToBin(time.second);
            time.minute = BCDToBin(time.minute);
            time.hour   = BCDToBin(time.hour & 0x7F); // Masking bit tertinggi (PM/AM) kalau perlu
            time.day    = BCDToBin(time.day);
            time.month  = BCDToBin(time.month);
            time.year   = BCDToBin(time.year);
        }

        // RTC biasanya cuma simpan 2 digit tahun (misal: 25 untuk 2025)
        // Kita perlu nebak abadnya. Untuk sekarang, hardcode abad 21 (2000-an).
        time.year += 2000; 

        return time;
        }
    }

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

        
    }
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

