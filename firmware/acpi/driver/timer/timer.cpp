#include "rossys.hpp"
#define PRINTK_MODULE_NAME "ACPI-TIMER"
#include "timer.hpp"
#include <logging.hpp>
#include "../../../../kernel/driver/pic/timer/pit.hpp"
#include "../../../../kernel/intidt/idt.hpp"
#include <task.hpp>
#include "../kernel/log/fbcon/fbcon.hpp"
#include "../kernel/driver/pic/pic.hpp"

// Export lapic tick counter/frequency so Sleep can use LAPIC as time source
volatile U64 ACPI::Timer::LapicTicks = 0;
U32 ACPI::Timer::LapicHz = 0;
U64 ACPI::Timer::RawApicHz = 0;
bool ACPI::Timer::UsingTscDeadline = false;
U64 ACPI::Timer::TscTicksPerSystemTick = 0; 
static U32 ApicTicksPerSystemTick = 0;
static U64 LastTscTimestamp = 0;
static U64 SavedTimerVector = 0;

// Simple LAPIC timer IRQ handler (file-scope). Increment lapic tick counter.
static void LapicOnIrqHandler(void *context) {
    U64 CurrentTsc = Arch::ASM::RdTSC();

    // Fix First Tick Logic
    if (LastTscTimestamp == 0 && ACPI::Timer::TscTicksPerSystemTick > 0) {
        LastTscTimestamp = CurrentTsc - ACPI::Timer::TscTicksPerSystemTick;
    }

    U64 delta = CurrentTsc - LastTscTimestamp;

    // --- Wall Clock Calculation (Pakai TSC biar akurat) ---
    if (ACPI::Timer::TscTicksPerSystemTick > 0) {
        U64 passedTicks = delta / ACPI::Timer::TscTicksPerSystemTick;
        
        // Jitter compensation
        if (passedTicks == 0 && delta > (ACPI::Timer::TscTicksPerSystemTick / 2)) {
             passedTicks = 1; 
        }
        
        PIT::ticks += passedTicks;
        ACPI::Timer::LapicTicks += passedTicks;
        LastTscTimestamp = CurrentTsc; 
    } else {
        PIT::ticks += 1;
        LastTscTimestamp = CurrentTsc;
    }

    // --- Hybrid Re-Arming ---
    if(!Tasking::SchedulerActive){
        if(ACPI::Timer::UsingTscDeadline){
            // TSC Mode: Pakai satuan TSC
            U64 NextTarget = Arch::ASM::RdTSC() + ACPI::Timer::TscTicksPerSystemTick;
            ACPI::Timer::SetTSCDeadline(NextTarget);
        } else {
            // [FIX] Legacy Mode: Pakai satuan APIC, JANGAN TSC!
            ACPI::Timer::SetOneShotMode(ApicTicksPerSystemTick);
        }
    }

    Tasking::SchedulerTick(context);
}

namespace ACPI {
    namespace Timer {

        VOID Arm(U64 ticksFromNow) {
            // Parameter ticksFromNow ini dikirim Scheduler dalam satuan "TSC Delta".
            // Karena Scheduler ngitungnya: TimeSlice * TscTicksPerSystemTick.

             if (UsingTscDeadline) {
                if (ticksFromNow < 1000) ticksFromNow = 1000; 
                U64 now = Arch::ASM::RdTSC();
                SetTSCDeadline(now + ticksFromNow);
            } else {
                // [FIX] Convert TSC Delta -> APIC Delta
                // Rumus: (TSC_Delta * APIC_Freq) / TSC_Freq
                // Atau lebih simpel pake ratio yang udah kita hitung
                
                U64 apicVal = 1000; // Safe default
                
                if (TscTicksPerSystemTick > 0 && ApicTicksPerSystemTick > 0) {
                     // Gunakan math 64-bit untuk presisi
                     // (TargetTSC * ApicPerTick) / TscPerTick
                     apicVal = (ticksFromNow * ApicTicksPerSystemTick) / TscTicksPerSystemTick;
                }
                
                // Safety clamp buat legacy timer (max 32-bit)
                if (apicVal > 0xFFFFFFFF) apicVal = 0xFFFFFFFF;
                if (apicVal < 100) apicVal = 100;

                SetOneShotMode((U32)apicVal); 
            }
        }

        // LAPIC timer register offsets
        #define LAPIC_REG_TIMER_LVT    0x320
        #define LAPIC_REG_TIMER_INIT   0x380
        #define LAPIC_REG_TIMER_CURR   0x390
        #define LAPIC_REG_TIMER_DIV    0x3E0

        // LVT Timer flags
        #define APIC_LVT_TIMER_MASK    (1U << 16)
        #define APIC_LVT_TIMER_PERIODIC (1U << 17)
         #define APIC_LVT_TIMER_TSC_DEADLINE (2U << 17) // Mode 2 (Binary 10)

        // Common divide configuration: encodings per Intel/AMD manuals.
        // 0x3 corresponds to divide value 16 on many implementations and is
        // a reasonable default for periodic tick rates.
        #define LAPIC_DIVIDE_BY_16 0x3

        void SetTSCDeadline(U64 futureTsc) {
            Arch::ASM::Mfence();
            Arch::MSR::Write(IA32_TSC_DEADLINE_MSR, futureTsc);
        }

        // Calibrate APIC timer against PIT and configure it for desiredHz.
        VOID InitializeLapicTimer(U8 Vector, U32 desiredHz, BOOL periodic) {
            if (desiredHz == 0) desiredHz = 100;
            
            SavedTimerVector = CONFIG_TIMER_HEXA_GLOBAL; 

            ACPI::Timer::LapicTicks = 0;
            ACPI::Timer::LapicHz = 0;
            ACPI::Timer::RawApicHz = 0;
            ACPI::Timer::UsingTscDeadline = false;
            ACPI::Timer::TscTicksPerSystemTick = 0;
            ApicTicksPerSystemTick = 0; // Reset
            LastTscTimestamp = 0;

            IDT::RegisterInterruptHandler(Vector, LapicOnIrqHandler);

            Printk::Write(Printk::Level::LOG_INFO, " LAPIC: Calibrating using PIT Polling...\n");

            // Setup PIT & LAPIC for Calibration
            Arch::ASM::Cli();
            Port::Outb(0x43, 0x34); 
            Port::Outb(0x40, 0xFF); 
            Port::Outb(0x40, 0xFF); 

            ACPI::LAPIC::LapicWrite(LAPIC_REG_TIMER_DIV, LAPIC_DIVIDE_BY_16);
            U32 lvt_calib = ((U32)Vector & 0xFF) | APIC_LVT_TIMER_MASK;
            ACPI::LAPIC::LapicWrite(LAPIC_REG_TIMER_LVT, lvt_calib);
            ACPI::LAPIC::LapicWrite(LAPIC_REG_TIMER_INIT, 0xFFFFFFFFu);

            const U16 PIT_TICKS_TO_WAIT = 11932; // ~10ms
            
            U16 startPit = PIT::ReadPITCounter();
            U16 currentPit = startPit;
            U64 startTsc = Arch::ASM::RdTSC();
            U32 startApic = ACPI::LAPIC::LapicRead(LAPIC_REG_TIMER_CURR);

            while (true) {
                currentPit = PIT::ReadPITCounter();
                U16 deltaPit;
                if (startPit >= currentPit) deltaPit = startPit - currentPit;
                else deltaPit = startPit + (0xFFFF - currentPit);

                if (deltaPit >= PIT_TICKS_TO_WAIT) break;
                Arch::ASM::PauseCPU();
            }

            U64 endTsc = Arch::ASM::RdTSC();
            U32 endApic = ACPI::LAPIC::LapicRead(LAPIC_REG_TIMER_CURR);
            Arch::ASM::Sti();

            // --- CALCULATE ---
            U32 deltaApic = startApic - endApic;
            U64 deltaTsc = endTsc - startTsc;
            U64 pitBase = 1193182;
            
            // Freq setelah Divider
            ACPI::Timer::RawApicHz = (U64)deltaApic * pitBase / PIT_TICKS_TO_WAIT;
            U64 measuredTscHz = deltaTsc * pitBase / PIT_TICKS_TO_WAIT;
            
            ACPI::Timer::LapicHz = desiredHz;

            if (desiredHz > 0) {
                // 1. Hitung tick untuk TSC (Angkanya Gede, ~20 Juta)
                ACPI::Timer::TscTicksPerSystemTick = measuredTscHz / desiredHz;
                
                // 2. [FIX] Hitung tick untuk APIC Legacy (Angkanya Kecil, ~625 Ribu)
                ApicTicksPerSystemTick = ACPI::Timer::RawApicHz / desiredHz;
            }

            Printk::Write(Printk::Level::LOG_INFO, 
                " LAPIC Calib: APIC=%u Hz, TSC=%llu Hz\n", 
                ACPI::Timer::RawApicHz, measuredTscHz);
            Printk::Write(Printk::Level::LOG_INFO, 
                " Ticks/SysTick: TSC=%llu, APIC=%u\n", 
                ACPI::Timer::TscTicksPerSystemTick, ApicTicksPerSystemTick);

            // --- MODE SELECTION ---
            BOOL TSCSupport = Arch::ASM::HasTSCDeadline();

            if(TSCSupport) {
                Printk::Write(Printk::Level::LOG_INFO, " LAPIC: TSC mode active!\n");
                ACPI::Timer::UsingTscDeadline = true;
                ACPI::LAPIC::LapicWrite(LAPIC_REG_TIMER_INIT, 0);

                U32 lvt = (Vector & 0xFF);
                lvt |= APIC_LVT_TIMER_TSC_DEADLINE;
                lvt &= ~APIC_LVT_TIMER_MASK; 

                ACPI::LAPIC::LapicWrite(LAPIC_REG_TIMER_LVT, lvt);
                Arch::ASM::Mfence();

                SetTSCDeadline(Arch::ASM::RdTSC() + ACPI::Timer::TscTicksPerSystemTick);
            } else {
                ACPI::Timer::UsingTscDeadline = false;
                
                // [FIX] Gunakan ApicTicksPerSystemTick!
                SetOneShotMode(ApicTicksPerSystemTick);
                Printk::Write(Printk::Level::LOG_INFO, " LAPIC: Legacy Mode Active!\n");
            }
        }

        U64 MicrosecondsToTicks(U64 us) {
            // Rumus: (us * Freq) / 1.000.000
            return (us * RawApicHz) / 1000000ULL;
        }

        U64 NanosecondsToTicks(U64 ns) {
            // Rumus: (ns * Freq) / 1.000.000.000
            // Hati-hati overflow kalau ns besar, tapi biasanya ns dipanggil untuk durasi kecil
            return (ns * RawApicHz) / 1000000000ULL;
        }

        VOID StopTimer() {
            // Mask interrupt (bit 16) dan set initial count 0
            U32 lvt = ACPI::LAPIC::LapicRead(LAPIC_REG_TIMER_LVT);
            lvt |= APIC_LVT_TIMER_MASK; 
            ACPI::LAPIC::LapicWrite(LAPIC_REG_TIMER_LVT, lvt);
            ACPI::LAPIC::LapicWrite(LAPIC_REG_TIMER_INIT, 0);
        }

        VOID SetOneShotMode(U64 tick) {
            StopTimer(); // Safety first

            // Set LVT ke One-Shot (Hapus bit Periodic, Hapus Mask)
            // Asumsi Vector sudah diketahui/disimpan, atau pass sebagai argumen.
            // Biasanya vector timer itu fix (misal 0x20 atau 0xFE).
            U32 vector = CONFIG_TIMER_HEXA_GLOBAL; // Sesuaikan dengan vector kamu
            U32 lvt = vector; // Periodic bit (bit 17) 0 = One-Shot
            
            ACPI::LAPIC::LapicWrite(LAPIC_REG_TIMER_LVT, lvt);
            ACPI::LAPIC::LapicWrite(LAPIC_REG_TIMER_DIV, LAPIC_DIVIDE_BY_16); // Pastikan divider sama
            
            // Tulis nilai hitungan mundur
            // LAPIC akan decrement ini sampai 0, lalu fire IRQ sekali.
            ACPI::LAPIC::LapicWrite(LAPIC_REG_TIMER_INIT, (U32)tick);
        }
    }
}
