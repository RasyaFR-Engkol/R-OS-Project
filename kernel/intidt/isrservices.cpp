#include "debug.hpp"
#include "rossys.hpp"
#define PRINTK_MODULE_NAME "ISRServ"
#include "idt.hpp"
#include "../log/printk/printk.hpp"
#include <rosval.h>

ABI_C void PageFaultHandler(UPTR faulting_address, U64 error_code) {
    Arch::ASM::Cli();

    // Expect crash panggil disini
    ExpectedCrash::ExpectCrash_T *EXP = ExpectedCrash::Instance;
    for(int i = 0; i < ExpectedCrash::InstanceCount; i++){
        if(EXP[i].ReportMustBeCalled != EXP[i].Counter){
            Printk::Write(Printk::LOG_EMERG, "EXPECTED CRASH: Function %s expected to crash after %llu calls. (called %llu times)\n",
                EXP[i].NameFunction,
                EXP[i].ReportMustBeCalled,
                EXP[i].Counter
            );
            // reset counter biar ga terus-terusan crash
            EXP[i].Counter = 0;
            break;
        }
    }

    // Log details at non-halting levels, then emit one EMERG that will dump stack and halt.
    Printk::Write(Printk::LOG_CRIT, "[ISR] Page Fault Exception!\n");
    Printk::Write(Printk::LOG_ERR, "       Faulting Address: %p\n", (void*)faulting_address);
    Printk::Write(Printk::LOG_ERR, "       Error Code: 0x%p\n", (void*)error_code);

    // Decode error code
    Printk::Write(Printk::LOG_ERR, "       Error Details:\n");
    Printk::Write(Printk::LOG_ERR, "         - Present: %s\n", (error_code & 0x1) ? "Yes" : "No");
    Printk::Write(Printk::LOG_ERR, "         - Write: %s\n", (error_code & 0x2) ? "Yes" : "No");
    Printk::Write(Printk::LOG_ERR, "         - User Mode: %s\n", (error_code & 0x4) ? "Yes" : "No");
    Printk::Write(Printk::LOG_ERR, "         - Reserved Bit Violation: %s\n", (error_code & 0x8) ? "Yes" : "No");
    Printk::Write(Printk::LOG_ERR, "         - Instruction Fetch: %s\n", (error_code & 0x10) ? "Yes" : "No");

    // Final panic message and halt will be handled by Printk when LOG_EMERG is used.
    Printk::Write(Printk::LOG_EMERG, "[ISR] System Halted due to Page Fault.\n");
}