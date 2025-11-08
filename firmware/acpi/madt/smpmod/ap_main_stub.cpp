#include <rosval.h>
#include <rossys.hpp>
#define PRINTK_MODULE_NAME "AppProcStub"
#include <logging.hpp>
#include <mm.hpp>
#include "smp.hpp"

// Weak C-linkage stub for AP entry. Replace with real implementation later.
extern "C" void ap_main_entry(U32 apic_id) {
    using namespace Printk;
    // Switch to the kernel PML4 so we no longer rely on the temporary identity map.
    asm volatile("mov %0, %%cr3" :: "r"(KernelPML4Phys) : "memory");
    Write(Level::LOG_INFO, "AP[%u]: entered ap_main_entry, halting\n", (unsigned)apic_id);
    ACPI::LAPIC::SMP::SignalApReady(apic_id);
    // Park the AP for now
    for (;;) {
        asm volatile ("hlt");
    }
}
