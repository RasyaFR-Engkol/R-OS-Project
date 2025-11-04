// Minimal implementations for stack protector symbols when building a freestanding kernel
// Provide __stack_chk_guard and __stack_chk_fail so -fstack-protector-strong links

extern "C" {
    // A non-zero guard value; kernel can randomize this at boot if desired.
    unsigned long __stack_chk_guard = 0xDEADBEEFBADC0FFFull;

    // Called when stack smashing is detected. Halt the CPU.
    __attribute__((noreturn)) void __stack_chk_fail(void) {
        // Disable interrupts and halt forever. Avoid calling other runtime code.
        asm volatile("cli");
        for (;;) {
            asm volatile("hlt");
        }
    }
}
