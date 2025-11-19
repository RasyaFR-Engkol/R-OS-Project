#define PRINTK_MODULE_NAME "ACPI-TIMER"
#include "timer.hpp"
#include <logging.hpp>
#include "../../../../kernel/driver/pic/timer/pit.hpp"
#include "../../../../kernel/intidt/idt.hpp"
#include <task.hpp>

// Export lapic tick counter/frequency so Sleep can use LAPIC as time source
volatile U64 ACPI::Timer::LapicTicks = 0;
U32 ACPI::Timer::LapicHz = 0;

// Simple LAPIC timer IRQ handler (file-scope). Increment lapic tick counter.
static void LapicOnIrqHandler(void *context) {
    ACPI::Timer::LapicTicks = ACPI::Timer::LapicTicks + 1;
    PIT::ticks = PIT::ticks + 1;
    Tasking::SchedulerTick(context);
}

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

            // Reset tick counters/frequency before calibration.
            ACPI::Timer::LapicTicks = 0;
            ACPI::Timer::LapicHz = 0;

            // Ensure divide config
            ACPI::LAPIC::LapicWrite(LAPIC_REG_TIMER_DIV, LAPIC_DIVIDE_BY_16);
            Printk::Write(Printk::Level::LOG_DEBUG,
                " LAPIC timer: set divide config (reg=0x%03x, value=0x%x)\n",
                LAPIC_REG_TIMER_DIV, LAPIC_DIVIDE_BY_16);

            // Put LVT into one-shot mode first (clear periodic), unmasked
            U32 lvt = (U32)Vector & 0xFF;
            ACPI::LAPIC::LapicWrite(LAPIC_REG_TIMER_LVT, lvt);
            Printk::Write(Printk::Level::LOG_DEBUG,
                " LAPIC timer: initial LVT write vector=0x%02x periodic=%s\n",
                (unsigned)Vector, periodic ? "true" : "false");

            // Register a simple IRQ handler that increments LapicTicks.
            // This ensures the kernel has a tick source when LAPIC timer
            // delivers interrupts on the given vector.
            IDT::RegisterInterruptHandler(Vector, LapicOnIrqHandler);
            Printk::Write(Printk::Level::LOG_DEBUG,
                " LAPIC timer: registered IRQ handler on vector 0x%02x\n",
                (unsigned)Vector);

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

            // Avoid floating point (SSE) in early kernel build: compute APIC Hz
            // using 64-bit integer math: apic_hz = delta * pit_hz / SAMPLE_PIT_TICKS
            U64 apic_hz_num = (U64)delta * (U64)pit_hz;
            U32 measured_apic_hz = (U32)(apic_hz_num / (U64)SAMPLE_PIT_TICKS);
            Printk::Write(Printk::Level::LOG_DEBUG,
                " LAPIC timer sample: startPit=%llu sampleTicks=%u pit_hz=%u before=0x%08x after=0x%08x delta=%u apic_hz=%llu\n",
                (unsigned long long)startPit,
                (unsigned)SAMPLE_PIT_TICKS,
                (unsigned)pit_hz,
                (unsigned)before,
                (unsigned)after,
                (unsigned)delta,
                (unsigned long long)(apic_hz_num / (U64)SAMPLE_PIT_TICKS));

            U32 initial_for_desired = 0x00100000u; // fallback
            if (measured_apic_hz > 0) {
                initial_for_desired = (U32)((U64)measured_apic_hz / (U64)desiredHz);
                if (initial_for_desired == 0) initial_for_desired = 1;
            }

            // LapicHz should represent the rate at which LapicTicks increments
            // (i.e., the requested/actual tick frequency, not the raw APIC clock).
            // Use the desiredHz as the logical tick rate exposed to users.
            ACPI::Timer::LapicHz = desiredHz;
            Printk::Write(Printk::Level::LOG_DEBUG,
                " LAPIC timer calibration: measured_hz=%u desired_hz=%u initial_count=0x%08x\n",
                (unsigned)ACPI::Timer::LapicHz,
                (unsigned)desiredHz,
                (unsigned)initial_for_desired);

            // Configure LVT for requested mode (periodic if desired)
            lvt = (U32)Vector & 0xFF;
            if (periodic) lvt |= APIC_LVT_TIMER_PERIODIC;
            ACPI::LAPIC::LapicWrite(LAPIC_REG_TIMER_LVT, lvt);
            Printk::Write(Printk::Level::LOG_DEBUG,
                " LAPIC timer: final LVT value=0x%08x (vector=0x%02x, periodic=%s)\n",
                (unsigned)lvt,
                (unsigned)Vector,
                periodic ? "true" : "false");

            ACPI::LAPIC::LapicWrite(LAPIC_REG_TIMER_INIT, initial_for_desired);
            Printk::Write(Printk::Level::LOG_DEBUG,
                " LAPIC timer: initial count set to 0x%08x\n",
                (unsigned)initial_for_desired);

            Printk::Write(Printk::Level::LOG_INFO, " LAPIC timer calibrated: desired=%uHz apic_hz=%u initial=0x%08x mode=%s\n",
                (unsigned)desiredHz, (unsigned)measured_apic_hz, (unsigned)initial_for_desired, periodic ? "periodic" : "one-shot");
        }

    }
}
