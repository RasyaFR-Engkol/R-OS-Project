/* Linker script for ROS: split low boot stub and higher-half kernel */
ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)

/* Virtual base for kernel proper and physical load base */
KERNEL_VMA = 0xffffffff80000000;
KERNEL_LMA = 0x00200000; /* 2MiB */

PHDRS {
    boottext   PT_LOAD;
    bootrodata PT_LOAD;
    bootdata   PT_LOAD;
    bootbss    PT_LOAD;
    text       PT_LOAD;
    rodata     PT_LOAD;
    data       PT_LOAD;
    bss        PT_LOAD;
}

SECTIONS {
    /* Low memory boot stub: identity mapped */
    . = 0x00100000; /* 1MiB */

    .boottext : {
        KEEP(*(.multiboot2_header))
        *(.boottext*)
    } :boottext

    .bootrodata : {
        *(.bootrodata*)
    } :bootrodata

    .bootdata : {
        *(.bootdata*)
    } :bootdata

    .bootbss : {
        *(.bootbss*)
    } :bootbss

    /* Higher-half kernel proper mapped at KERNEL_VMA, loaded at KERNEL_LMA */
    . = KERNEL_VMA;
    . = ALIGN(4096); /* For page protection and alignment */

    .text : AT (KERNEL_LMA + (ADDR(.text) - KERNEL_VMA)) {
        __text_start = .;
        __kernel_virt_start = .;
        *(.text .text.*)
        . = ALIGN(4096);
        __text_end = .;
    } :text

    .rodata : AT (KERNEL_LMA + (ADDR(.rodata) - KERNEL_VMA)) {
        __rodata_start = .;
        *(.rodata .rodata.*)

        . = ALIGN(8); /* Pointer 64-bit butuh align 8 byte */
        PROVIDE(__ksymtab_start = .);
        KEEP(*(__ksymtab))       /* Array of KernelSymbol structs */
        PROVIDE(__ksymtab_end = .);

        . = ALIGN(1);
        KEEP(*(__ksymtab_strings)) /* String names dari symbolnya */
        . = ALIGN(4096);
        __rodata_end = .;
    } :rodata

    .data : AT (KERNEL_LMA + (ADDR(.data) - KERNEL_VMA)) {
        __data_start = .;
        *(.data .data.*)
        . = ALIGN(4096);
        __data_end = .;
    } :data

    .init_array : AT (KERNEL_LMA + (ADDR(.init_array) - KERNEL_VMA)) {
        PROVIDE(__init_array_start = .);
        *(.init_array)
        PROVIDE(__init_array_end = .);
    } :data

    .bss : AT (KERNEL_LMA + (ADDR(.bss) - KERNEL_VMA)) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON) /* <--- WAJIB DITAMBAHIN CUK */
        . = ALIGN(4096);
        __bss_end = .;
    } :bss

    /DISCARD/ : { *(.eh_frame) *(.eh_frame_hdr) *(.comment) }

    /* Useful symbols */
    __kernel_phys_start = LOADADDR(.text);
    __kernel_phys_end   = LOADADDR(.bss) + SIZEOF(.bss);
    __kernel_virt_end   = ADDR(.bss) + SIZEOF(.bss);
}