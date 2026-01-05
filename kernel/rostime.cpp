// rostime.cpp - Arch::Time implementation moved out of header to avoid weird inline/static issues
#include <rossys.hpp>
#include <rosval.h>
#include "../firmware/acpi/driver/timer/timer.hpp"
#include "../kernel/driver/pic/timer/pit.hpp"
#include <logging.hpp>

namespace Arch {
    namespace Time {

        bool LapicTimingActive() {
            return ACPI::Timer::LapicHz != 0;
        }

        U64 NowTicks() {
            if (LapicTimingActive()) return ACPI::Timer::LapicTicks;
            return PIT::ticks;
        }

        U32 TickHz() {
            if (LapicTimingActive()) {
                return ACPI::Timer::LapicHz;
            }
            U16 reload = PIT::PITReload;
            if (reload == 0) return 100;
            const U32 PIT_BASE = 1193182U;
            return (U32)(PIT_BASE / (U32)reload);
        }

        void SleepTicks(U64 ticks) {
            U64 target = NowTicks() + ticks;
            if (Arch::AreInterruptsEnabled()) {
                while (NowTicks() < target) asm volatile ("hlt");
            } else {
                while (NowTicks() < target) Arch::CPURelax();
            }
        }

        void SleepMs(U64 ms) {
            U32 hz = TickHz();
            if (hz == 0) hz = 100;
            U64 ticks = (ms * (U64)hz + 999) / 1000ULL; // ceil
            SleepTicks(ticks);
        }

        static U64 TSCHz = 0;

        static inline U64 ReadTSC() {
            U32 lo, hi;
            asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
            return ((U64)hi << 32) | lo;
        }

        void CalibrateTSC() {
            if (TSCHz != 0) return;
            
            // Pastikan interrupt aktif agar SleepMs jalan (karena SleepMs butuh timer interrupt)
            if (!Arch::AreInterruptsEnabled()) {
                Printk::Write(Printk::Level::LOG_WARNING, "Arch::Time: Cannot calibrate TSC with interrupts disabled! Using fallback 3GHz assumption.\n");
                TSCHz = 3000000000ULL; // Asumsi 3GHz
                return;
            }

            Printk::Write(Printk::Level::LOG_INFO, "Arch::Time: Calibrating TSC...\n");
            
            U64 start_tsc = ReadTSC();
            SleepMs(50); // Sleep 50ms
            U64 end_tsc = ReadTSC();
            
            TSCHz = (end_tsc - start_tsc) * 20;
            Printk::Write(Printk::Level::LOG_INFO, "Arch::Time: TSC Frequency: %llu Hz\n", TSCHz);
        }

        void SleepUs(U64 us) {
            if (us == 0) return;
            if (TSCHz == 0) CalibrateTSC();

            // Jika delay cukup panjang (> 1ms), gunakan SleepMs biar hemat CPU
            if (us >= 1000) {
                SleepMs(us / 1000);
                us %= 1000;
                if (us == 0) return;
            }

            U64 start = ReadTSC();
            U64 cycles = (us * TSCHz) / 1000000;
            
            while ((ReadTSC() - start) < cycles) {
                asm volatile("pause");
            }
        }

        void SleepNs(U64 ns) {
            if (ns < 5000) { 
                // Kalau cuma < 5us, busy wait aja (overhead syscall/interrupt lebih mahal)
                SleepUs((ns + 999) / 1000); // ceil ke us
                return;
            }

            // Hitung target TSC
            // TSCHz udah kamu dapet dari kalibrasi di rostime.cpp
            U64 deltaTsc = (ns * TSCHz) / 1000000000ULL;
            U64 target = ReadTSC() + deltaTsc;

            // Set Hardware Timer
            ACPI::Timer::SetTSCDeadline(target);

            // Halt CPU, tunggu interrupt bangunin
            asm volatile("hlt");
        }

        void Sleep(U64 ms) { SleepMs(ms); }
        void SleepSeconds(U64 s) { SleepMs(s * 1000); }

        U64 GetTickCount(){ return PIT::ticks; }
        U64 GetSecCount(){ return (PIT::ticks / 100); }

        static const int days_before_month[] = {
            0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
        };

        bool is_leap_year(uint32_t year) {
            return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        }

        U32 RTCToEpoch(Arch::CMOS::RTCTime T){
            U32 Days = 0;
            U32 Year = T.year;

            for(U32 y = 1970; y < Year; y++){
                Days += 365;
                if(is_leap_year(y)) Days += 1;
            }

            Days += days_before_month[T.month - 1];

            if(T.month > 2 && is_leap_year(Year)){
                Days += 1;
            }

            Days += (T.day - 1);

            U32 TotalSeconds = (Days * 86400) +
                              (T.hour * 3600) +
                              (T.minute * 60) +
                              T.second;

            return TotalSeconds;
        }

    }
}
