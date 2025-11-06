#define PRINTK_MODULE_NAME "ACPI-TIMER"
#include "timer.hpp"
#include <logging.hpp>
#include "../../../../kernel/driver/pic/timer/pit.hpp"

namespace ACPI {
    namespace Timer {

        // LAPIC timer register offsets
        #define LAPIC_REG_TIMER_LVT    0x320
        #define LAPIC_REG_TIMER_INIT   0x380
        #define LAPIC_REG_TIMER_CURR   0x390
        #define LAPIC_REG_TIMER_DIV    0x3E0

        // LVT Timer flags
        #define APIC_LVT_TIMER_MASK    (1U << 16)
        #define APIC_LVT_TIMER_PERIODIC (1U << 17)

        // Common divide configuration: encodings per Intel/AMD manuals.
        // 0x3 corresponds to divide value 16 on many implementations and is
        // a reasonable default for periodic tick rates.
        #define LAPIC_DIVIDE_BY_16 0x3

        // Calibrate APIC timer against PIT and configure it for desiredHz.
        VOID InitializeLapicTimer(U8 Vector, U32 desiredHz, BOOL periodic) {
            using namespace PIT;

            if (desiredHz == 0) desiredHz = 100; // default

            // Ensure divide config
            ACPI::LAPIC::LapicWrite(LAPIC_REG_TIMER_DIV, LAPIC_DIVIDE_BY_16);

            // Put LVT into one-shot mode first (clear periodic), unmasked
            U32 lvt = (U32)Vector & 0xFF;
            ACPI::LAPIC::LapicWrite(LAPIC_REG_TIMER_LVT, lvt);

            // Start with a large initial count so it won't wrap during measurement
            const U32 START_COUNT = 0xFFFFFFFFu;
            ACPI::LAPIC::LapicWrite(LAPIC_REG_TIMER_INIT, START_COUNT);

            // Measure APIC ticks over a small PIT interval (e.g., 5 PIT ticks)
            const U32 SAMPLE_PIT_TICKS = 5;
            volatile U64 startPit = PIT::ticks;
            // Wait until PIT ticks advances (ensure PIT is running at known freq)
            while (PIT::ticks == startPit) {}
            startPit = PIT::ticks;
            U32 before = ACPI::LAPIC::LapicRead(LAPIC_REG_TIMER_CURR);

            // Wait for SAMPLE_PIT_TICKS PIT ticks
            while (PIT::ticks < startPit + SAMPLE_PIT_TICKS) {}

            U32 after = ACPI::LAPIC::LapicRead(LAPIC_REG_TIMER_CURR);

            U32 delta = 0;
            if (before >= after) delta = before - after;
            else delta = (before + (0xFFFFFFFFu - after));

            // PIT frequency is PIT::PITReload-derived. PIT::PITReload holds divisor; base freq 1193182
            const U32 PIT_BASE = 1193182u;
            U32 pit_hz = (PITReload == 0) ? 100 : (PIT_BASE / PITReload);

            double elapsed_sec = (double)SAMPLE_PIT_TICKS / (double)pit_hz;
            double apic_hz = (double)delta / elapsed_sec;

            U32 initial_for_desired = 0x00100000u; // fallback
            if (apic_hz > 0.0) {
                initial_for_desired = (U32)(apic_hz / (double)desiredHz);
                if (initial_for_desired == 0) initial_for_desired = 1;
            }

            // Configure LVT for requested mode (periodic if desired)
            lvt = (U32)Vector & 0xFF;
            if (periodic) lvt |= APIC_LVT_TIMER_PERIODIC;
            ACPI::LAPIC::LapicWrite(LAPIC_REG_TIMER_LVT, lvt);

            ACPI::LAPIC::LapicWrite(LAPIC_REG_TIMER_INIT, initial_for_desired);

            Printk::Write(Printk::Level::LOG_INFO, " LAPIC timer calibrated: desired=%uHz apic_hz=%.0f initial=0x%08x mode=%s\n",
                (unsigned)desiredHz, apic_hz, (unsigned)initial_for_desired, periodic ? "periodic" : "one-shot");
        }

    }
}
