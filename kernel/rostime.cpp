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
                Printk::Write(Printk::Level::LOG_DEBUG,
                    " Arch::Time::TickHz: using LAPIC timing at %u Hz\n",
                    (unsigned)ACPI::Timer::LapicHz);
                return ACPI::Timer::LapicHz;
            }
            U16 reload = PIT::PITReload;
            if (reload == 0) return 100;
            const U32 PIT_BASE = 1193182U;
            return (U32)(PIT_BASE / (U32)reload);
        }

        void SleepTicks(U64 ticks) {
            Printk::Write(Printk::Level::LOG_DEBUG,
                " Arch::Time::SleepTicks: sleeping for %llu ticks\n",
                (unsigned long long)ticks);
            U64 target = NowTicks() + ticks;
            if (Arch::AreInterruptsEnabled()) {
                while (NowTicks() < target) asm volatile ("hlt");
            } else {
                while (NowTicks() < target) Arch::CPURelax();
            }

            Printk::Write(Printk::Level::LOG_DEBUG,
                " Arch::Time::SleepTicks: woke up at %llu ticks (target was %llu)\n",
                (unsigned long long)NowTicks(),
                (unsigned long long)target);
        }

        void SleepMs(U64 ms) {
            Printk::Write(Printk::Level::LOG_DEBUG,
                " Arch::Time::SleepMs: sleeping for %llu ms\n",
                (unsigned long long)ms);
            U32 hz = TickHz();
            if (hz == 0) hz = 100;
            U64 ticks = (ms * (U64)hz + 999) / 1000ULL; // ceil
            SleepTicks(ticks);
        }

        void Sleep(U64 ms) { SleepMs(ms); }
        void SleepSeconds(U64 s) { SleepMs(s * 1000); }

        U64 GetTickCount(){ return PIT::ticks; }
        U64 GetSecCount(){ return (PIT::ticks / 100); }

    }
}
