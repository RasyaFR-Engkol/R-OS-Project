#include <rosval.h>
#include <rossys.hpp>
#include <serial.hpp>
#include <mm.hpp>
#include "../driver/pic/pic.hpp"
#include "idt.hpp"
// Need LAPIC state (g_LapicVirtualBase) to decide per-interrupt EOI
#include "../../firmware/acpi/madt/madt.hpp"
// For context building and scheduler interaction
#include <cpu_context.hpp>
#include <string.hpp>
#include "../task/task.hpp"

extern "C" void Arch_SwitchStackAndResume(void* new_sp, void* resume_ip);
extern "C" void Scheduler_IretTrampoline();

// Common IRQ dispatcher called by asm stubs with irq number [0..207]
// Forward-declare LAPIC helper provided by ACPI MADT module so IRQ path
// can send EOI without remapping the register itself.
namespace ACPI { namespace LAPIC { VOID LapicWrite(U32 RegOffset, U32 Value); } }

static inline void LAPIC_SendEOI(U8 irq) {
    // EOI register offset is 0xB0; write 0 to signal end-of-interrupt.
    ACPI::LAPIC::LapicWrite(0x0B0, 0);
}

ABI_C VOID IrqDispatch(U64 irq) {
    U8 vector = 0x20 + (U8)irq; // hardware IRQ vectors base at 0x20
    
    // Panggil handler yang terdaftar
    IDT::InvokeInterruptHandler(vector);
    
    // ATURAN: send EOI to LAPIC when LAPIC is initialized. If the system
    // is still routing legacy INTx through the 8259 PIC, also send EOI to
    // the PIC for IRQs that originate from it (IRQ 0..15). This avoids the
    // problem where a LAPIC-delivered interrupt (e.g. LAPIC timer) arrives
    // while PIC is still marked active and would otherwise only get a
    // PIC EOI instead of a LAPIC EOI.
    if (ACPI::LAPIC::g_LapicVirtualBase != nullptr) {
        //erial::Printf("Notice: Sending EOI to LAPIC for IRQ %u .\n", irq);
        LAPIC_SendEOI((U8)irq);
    }

    // If legacy PIC is still in use, only send EOI to the PIC for IRQs in
    // the ISA range (0..15). Sending PIC EOIs for arbitrary high irq numbers
    // (like 206) is incorrect and caused the issue observed earlier.
    if (PIC::G_StillLegacyINTx && irq < 16) {
        //erial::Printf("Notice: Sending EOI to PIC for IRQ %u .\n", irq);
        PIC::SendEOI((U8)irq);
    }
}

// New wrapper called from irq stub: receives raw stack pointer where the
// irq stub pushed GPRs and where the CPU placed the hardware frame.
ABI_C VOID IrqDispatchWithRawStack(U64 irq, void* raw_sp) {
    U8 vector = 0x20 + (U8)irq;

    // Send EOIs like the original IrqDispatch
    if (ACPI::LAPIC::g_LapicVirtualBase != nullptr) {
        LAPIC_SendEOI((U8)irq);
    }
    if (PIC::G_StillLegacyINTx && irq < 16) {
        PIC::SendEOI((U8)irq);
    }

    // Invoke registered handler for this vector (same as old path)
    IDT::InvokeInterruptHandler(vector, raw_sp);

    // NOTE: scheduling from inside the IRQ handler is temporarily disabled
    // to avoid stability issues while the scheduler implementation matures.
    // We preserved the context to allow an external scheduler to run later.
    // Increment a simple tick counter for diagnostics.
    static volatile U64 irq_ticks = 0;
    (void)__atomic_fetch_add(&irq_ticks, 1ULL, __ATOMIC_RELAXED);
    (void)irq_ticks;

    return;
}