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
VOLATILE U64 ACPI::Timer::LapicTicks = 0;
U32 ACPI::Timer::LapicHz = 0;
U64 ACPI::Timer::RawApicHz = 0;
VOLATILE BOOL ACPI::Timer::UsingTscDeadline = false;
U64 ACPI::Timer::TscTicksPerSystemTick = 0; 
static VOLATILE U32 ApicTicksPerSystemTick = 0;
static VOLATILE U64 LastTscTimestamp = 0;
static VOLATILE U64 SavedTimerVector = 0;
U64 ACPI::Timer::TSCFrequencyHz = 0;
U64 ACPI::Timer::BootTSC = 0;

// Simple LAPIC timer IRQ handler (file-scope). Increment lapic tick counter.
// Tambah variable static buat nyimpen sisa recehan tick
static U64 TscAccumulator = 0; 

static void LapicOnIrqHandler(void *context) {
    U64 CurrentTsc = Arch::ASM::RdTSC();

    // Fix First Tick Logic
    if (LastTscTimestamp == 0 && ACPI::Timer::TscTicksPerSystemTick > 0) {
        LastTscTimestamp = CurrentTsc - ACPI::Timer::TscTicksPerSystemTick;
    }

    // Hitung delta raw TSC
    U64 delta = CurrentTsc - LastTscTimestamp;
    
    // Update timestamp SEKARANG (sebelum delta diapa-apain)
    LastTscTimestamp = CurrentTsc; 

    if (ACPI::Timer::TscTicksPerSystemTick > 0) {
        // Masukkan delta ke celengan (Accumulator)
        TscAccumulator += delta;

        // Cek celengan: Udah cukup buat beli 1 Tick belum?
        U64 passedTicks = TscAccumulator / ACPI::Timer::TscTicksPerSystemTick;
        
        if (passedTicks > 0) {
            PIT::ticks += passedTicks;
            ACPI::Timer::LapicTicks += passedTicks;
            
            // Kurangi celengan dengan jumlah yang sudah dicairkan jadi Tick
            // Sisa baginya (remainder) tetep disimpen buat next interrupt!
            TscAccumulator -= (passedTicks * ACPI::Timer::TscTicksPerSystemTick);
        }
        // Note: Hapus logic 'if(passedTicks < 1) passedTicks = 1' yang lama.
        // Biarkan 0 kalau emang belum sampe 1 tick, nanti diakumulasi di next interrupt.
    } else {
        PIT::ticks += 1;
        // Fallback kalau kalibrasi gagal
    }

    // Prevent deadline mode starvation: Kalau kita pakai TSC Deadline, pastikan timer tetap jalan di tiap tick walaupun ada delay di handler ini.
    if(!Tasking::SchedulerActive){
        if(ACPI::Timer::UsingTscDeadline){
            U64 NextTarget = Arch::ASM::RdTSC() + ACPI::Timer::TscTicksPerSystemTick;
            ACPI::Timer::SetTSCDeadline(NextTarget);
        } else {
            ACPI::Timer::SetOneShotMode(ApicTicksPerSystemTick);
        }
    }

    Tasking::SchedulerTick(context);
}

namespace ACPI {
    namespace Timer {

        static inline U64 ScaleUint64(U64 Value, U64 Mul, U64 Div) {
            U64 Result;
            U64 Remainder; // Dummy buat nampung sisa bagi
            
            asm volatile (
                "mul %3;"      // Instruksi MUL: RAX * Mul -> Disimpan di RDX:RAX (128-bit)
                "div %4;"      // Instruksi DIV: RDX:RAX / Div -> Hasil di RAX, Sisa di RDX
                : "=a"(Result), "=d"(Remainder) 
                : "a"(Value), "r"(Mul), "r"(Div)
                : "cc" // Kasih tau compiler kalau flag register berubah
            );
            return Result;
        }

        VOID Arm(U64 ticksFromNow) {
            // Safety delta buat KVM/QEMU biar timer ga kelewat
            if (ticksFromNow < 2000) ticksFromNow = 2000;

             if (UsingTscDeadline) {
                U64 now = Arch::ASM::RdTSC();
                SetTSCDeadline(now + ticksFromNow);
            } else {
                // [FIXED] Ganti __int128 pake Assembly Helper
                // Rumus: (ticksFromNow * ApicTicksPerSystemTick) / TscTicksPerSystemTick
                
                U64 apicVal = 1000;

                // Pastikan pembagi tidak nol (anti crash)
                if (TscTicksPerSystemTick > 0) {
                     apicVal = ScaleUint64(ticksFromNow, ApicTicksPerSystemTick, TscTicksPerSystemTick);
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
            
            SavedTimerVector = Vector;

            ACPI::Timer::LapicTicks = 0;
            ACPI::Timer::LapicHz = 0;
            ACPI::Timer::RawApicHz = 0;
            ACPI::Timer::UsingTscDeadline = false;
            ACPI::Timer::TscTicksPerSystemTick = 0;
            ApicTicksPerSystemTick = 0; // Reset
            LastTscTimestamp = 0;
            TscAccumulator = 0;

            ACPI::Timer::BootTSC = Arch::ASM::RdTSC();

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
            ACPI::Timer::TSCFrequencyHz = measuredTscHz;
            ACPI::Timer::LapicHz = desiredHz;

            if (desiredHz > 0) {
                ACPI::Timer::TscTicksPerSystemTick = measuredTscHz / desiredHz;
                
                if (ACPI::Timer::RawApicHz < desiredHz) ACPI::Timer::RawApicHz = desiredHz * 1000; 
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
                Printk::Write(Printk::Level::LOG_INFO, " LAPIC: TSC Deadline Mode Enabled! (Tickless Ready)\n");
                ACPI::Timer::UsingTscDeadline = true;
                
                // Pastikan INIT 0 di mode TSC Deadline
                ACPI::LAPIC::LapicWrite(LAPIC_REG_TIMER_INIT, 0); 
                Arch::ASM::Mfence();

                U32 lvt = (Vector & 0xFF);
                lvt |= APIC_LVT_TIMER_TSC_DEADLINE;
                lvt &= ~APIC_LVT_TIMER_MASK; // Unmask

                ACPI::LAPIC::LapicWrite(LAPIC_REG_TIMER_LVT, lvt);
                Arch::ASM::Mfence();

                // Kickstart timer pertama kali!
                SetTSCDeadline(Arch::ASM::RdTSC() + ACPI::Timer::TscTicksPerSystemTick);
            } else {
                ACPI::Timer::UsingTscDeadline = false;
                Printk::Write(Printk::Level::LOG_INFO, " LAPIC: Legacy One-Shot Mode (Fallback)\n");
                SetOneShotMode(ApicTicksPerSystemTick);
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
            StopTimer();
            // [FIX] Gunakan SavedTimerVector yang kita dapet dari Initialize
            U32 vector = SavedTimerVector & 0xFF; 
            
            ACPI::LAPIC::LapicWrite(LAPIC_REG_TIMER_LVT, vector); // One Shot (Bit 17=0)
            ACPI::LAPIC::LapicWrite(LAPIC_REG_TIMER_DIV, LAPIC_DIVIDE_BY_16);
            ACPI::LAPIC::LapicWrite(LAPIC_REG_TIMER_INIT, (U32)tick);
        }

        VOID GetTimeSinceBoot(U64 *RetSeconds, U64 *SubRetSeconds){
            if(TSCFrequencyHz == 0){
                *RetSeconds = 0;
                *SubRetSeconds = 0;
                return;
            }

            U64 Current = Arch::ASM::RdTSC();
            U64 diff = 0;

            if(Current >= BootTSC){
                diff = Current - BootTSC;
            } 

            *RetSeconds = diff / TSCFrequencyHz;

            U64 Remainder = diff % TSCFrequencyHz;
            *SubRetSeconds = (Remainder * 1000000ULL) / TSCFrequencyHz;
        }

        U64 MillisecondsToTicks(U64 ms) {
            // Rumus: (ms * Freq) / 1000
            // Menggunakan RawApicHz (Frequency Timer)
            return (ms * RawApicHz) / 1000ULL;
        }
    }
}
