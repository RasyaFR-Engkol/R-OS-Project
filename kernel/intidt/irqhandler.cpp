#include <rosval.h>
#include <rossys.hpp>
#include <serial.hpp>
#include <mm.hpp>
#include "../driver/pic/pic.hpp"
#include "idt.hpp"

// Common IRQ dispatcher called by asm stubs with irq number [0..207]
static inline void LAPIC_SendEOI() {
    // Lazily map Local APIC MMIO (phys 0xFEE00000) at a dedicated VA (not in HHDM), then write EOI at +0xB0.
    static volatile U32* s_lapic_eoi = nullptr;
    static volatile U32* s_lapic_regs = nullptr;
    if (!s_lapic_eoi) {
        const UPTR LAPIC_PHYS_BASE = 0xFEE00000u;
        // Allocate a fresh virtual page from the kernel VA pool to avoid clashing with 2MiB HHDM huge pages
        void* va = PageAlloc::VirtualAllocPages(1);
        if (!va) {
            Serial::Write("[APIC] ERROR: Failed to allocate VA for Local APIC MMIO page\n");
            return;
        }
        // Map one 4KiB page for LAPIC with cache disabled attributes
        PFLAGS flags = PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT;
        if (!PageAlloc::MapPages(KernelPML4, LAPIC_PHYS_BASE, (UPTR)va, 1, flags)) {
            Serial::Write("[APIC] ERROR: Failed to map Local APIC MMIO page\n");
            // return VA to pool if mapping failed
            PageAlloc::VirtualFreePages(va, 1);
            return; // avoid PF; better to lose an EOI than triple-fault
        }
        s_lapic_regs = (volatile U32*)va;
        s_lapic_eoi  = s_lapic_regs + (0xB0u >> 2);
    }
    *s_lapic_eoi = 0;
}

ABI_C VOID IrqDispatch(U64 irq) {
    U8 vector = 0x20 + (U8)irq; // hardware IRQ vectors base at 0x20
    // Call registered handler if present
    IDT::InvokeInterruptHandler(vector);
    // Send EOI: use legacy PIC for 0..15, Local APIC for >=16 (IOAPIC/MSI/MSI-X)
    if (irq < 16) {
        PIC::SendEOI((U8)irq);
    } else {
        LAPIC_SendEOI();
    }
}