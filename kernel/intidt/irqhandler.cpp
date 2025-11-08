#include <rosval.h>
#include <rossys.hpp>
#include <serial.hpp>
#include <mm.hpp>
#include "../driver/pic/pic.hpp"
#include "idt.hpp"

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
    
    // ATURAN BARU: SELALU kirim EOI ke LAPIC untuk SEMUA 
    // interrupt hardware (GSI, MSI, LAPIC Timer, dll.)
    if(PIC::G_StillLegacyINTx){
        PIC::SendEOI((U8)irq);
    } else {
        LAPIC_SendEOI((U8)irq);
    }
}