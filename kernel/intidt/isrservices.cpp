#include "debug.hpp"
#include "rossys.hpp"
#define PRINTK_MODULE_NAME "ISRServ"
#include "idt.hpp"
#include "../log/printk/printk.hpp"
#include <rosval.h>

ABI_C void PageFaultHandler(UPTR faulting_address, U64 error_code) {
    Arch::ASM::Cli();

    // Consolidate all fault information into a single emergency log call.
    const CHAR8* present = (error_code & 0x1) ? "Yes" : "No";
    const CHAR8* write = (error_code & 0x2) ? "Yes" : "No";
    const CHAR8* user = (error_code & 0x4) ? "Yes" : "No";
    const CHAR8* reserved = (error_code & 0x8) ? "Yes" : "No";
    const CHAR8* instr = (error_code & 0x10) ? "Yes" : "No";

    Printk::Write(Printk::LOG_EMERG,
        "[ISR] Page Fault Exception!\n"
        "       Faulting Address: %p\n"
        "       Error Code: 0x%llx\n"
        "       Error Details:\n"
        "         - Present: %s\n"
        "         - Write: %s\n"
        "         - User Mode: %s\n"
        "         - Reserved Bit Violation: %s\n"
        "         - Instruction Fetch: %s\n"
        "[ISR] System Halted due to Page Fault.\n",
        (void*)faulting_address,
        (unsigned long long)error_code,
        present, write, user, reserved, instr);
}