#include <rosval.h>
#include <rossys.hpp>
#include <serial.hpp>
#include <mm.hpp>
#include "../driver/pic/pic.hpp"
#include "idt.hpp"
// Need LAPIC state (g_LapicVirtualBase) to decide per-interrupt EOI
#include "../../firmware/acpi/madt/madt.hpp"

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