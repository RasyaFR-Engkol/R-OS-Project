#include <rosval.h>
#include <rossys.hpp>
#include <serial.hpp>
#include "../driver/pic/pic.hpp"
#include "idt.hpp"

// Common IRQ dispatcher called by asm stubs with irq number [0..99]
ABI_C VOID IrqDispatch(U64 irq) {
    U8 vector = 0x20 + (U8)irq; // hardware IRQ vectors base at 0x20
    // Call registered handler if present
    IDT::InvokeInterruptHandler(vector);
    // Send EOI for legacy PIC lines only (0..15). Ignore others for now.
    if (irq < 16) {
        PIC::SendEOI((U8)irq);
    }
}