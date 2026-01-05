#include <rosval.h>
#include "../../acpi.hpp"
#include "../madt.hpp"
#include "rossys.hpp"
#define PRINTK_MODULE_NAME "APMain"
#include <logging.hpp>

ABI_C VOID APMain(U64 ApicId){
    ACPI::LAPIC::InitializeLAPIC();
    
    Arch::Sti();

    // g_CpusAwake.fetch_add(1); // uncomment jika butuh sinkronisasi

    Printk::Write(Printk::Level::LOG_NOTICE, "[AP %u] AP is awake and running!\n", (unsigned)ApicId);

    while(TRUE){
        Arch::Cli();
        Arch::HaltCPU();
        Arch::Sti();
    }
}