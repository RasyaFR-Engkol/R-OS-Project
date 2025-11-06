/* Linker script for AP trampoline (16-bit Real Mode to Long Mode) */
ENTRY(trampoline_start)
OUTPUT_FORMAT(elf64-x86-64)

/* Physical load address for trampoline. Must be 4 KiB aligned and < 1MiB. */
TRAMPOLINE_BASE = 0x00008000; /* Matches ORG 0x8000 in assembly */

SECTIONS {
    /* Place everything contiguously starting at physical 0x8000 */
    . = TRAMPOLINE_BASE;
    .trampoline : {
        trampoline_start = .;
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.data .data.*)
        *(.bss .bss.*)
        trampoline_end = .;
    }

    /DISCARD/ : { *(.eh_frame) *(.comment) }

    trampoline_size = trampoline_end - trampoline_start;
}
