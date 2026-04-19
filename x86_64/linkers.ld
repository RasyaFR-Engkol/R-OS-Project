/* Linker script for ROS: split low boot stub and higher-half kernel */
ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)

/* Virtual base for kernel proper and physical load base */
KERNEL_VMA = 0xffffffff80000000;
KERNEL_LMA = 0x01000000; /* 16MiB - Aman dari area ACPI di ~2MB */

/* Pisahkan segment untuk file ELF biar nggak terjadi padding raksasa */
PHDRS {
    boot_rx   PT_LOAD FLAGS(5); /* Read + Execute */
    boot_rw   PT_LOAD FLAGS(6); /* Read + Write */
    kernel_rx PT_LOAD FLAGS(5); /* Read + Execute */
    kernel_rw PT_LOAD FLAGS(6); /* Read + Write */
}

SECTIONS {
    /* ==================================================================== */
    /* BAGIAN 1: LOW MEMORY BOOT STUB (Identity Mapped)                     */
    /* ==================================================================== */
    
    /* Mulai di 1MiB (0-nya 5 biji, jangan kelebihan!) */
    . = 0x00100000; 

    .boottext : {
        /* Multiboot2 header WAJIB ada di urutan paling atas */
        KEEP(*(.multiboot2_header))
        *(.boottext .boottext.*)
    } :boot_rx

    .bootrodata : {
        *(.bootrodata .bootrodata.*)
    } :boot_rx

    /* Align ke 4K sebelum masuk area Writeable */
    . = ALIGN(4096);

    .bootdata : {
        *(.bootdata .bootdata.*)
    } :boot_rw

    .bootbss : {
        *(.bootbss .bootbss.*)
    } :boot_rw


    /* ==================================================================== */
    /* BAGIAN 2: HIGHER-HALF KERNEL PROPER                                  */
    /* ==================================================================== */
    
    /* Lompat ke Virtual Memory Address (VMA) */
    . = KERNEL_VMA;
    . = ALIGN(4096); 

    /* AT(...) mendefinisikan Load Memory Address (LMA) alias fisik-nya */
    .text : AT(KERNEL_LMA + (ADDR(.text) - KERNEL_VMA)) {
        __text_start = .;
        __kernel_virt_start = .;
        *(.text .text.*)
        . = ALIGN(4096);
        __text_end = .;
    } :kernel_rx

    .rodata : AT(KERNEL_LMA + (ADDR(.rodata) - KERNEL_VMA)) {
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
    } :kernel_rx

    .data : AT(KERNEL_LMA + (ADDR(.data) - KERNEL_VMA)) {
        __data_start = .;
        *(.data .data.*)
        . = ALIGN(4096);
        __data_end = .;
    } :kernel_rw

    .init_array : AT(KERNEL_LMA + (ADDR(.init_array) - KERNEL_VMA)) {
        PROVIDE(__init_array_start = .);
        KEEP(*(.init_array .init_array.*))
        PROVIDE(__init_array_end = .);
        . = ALIGN(4096);
    } :kernel_rw

    .bss : AT(KERNEL_LMA + (ADDR(.bss) - KERNEL_VMA)) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON) /* <--- Wajib, biar variabel global yg gak diinisialisasi masuk sini */
        . = ALIGN(4096);
        __bss_end = .;
    } :kernel_rw

    /DISCARD/ : { 
        *(.eh_frame) 
        *(.eh_frame_hdr) 
        *(.comment) 
        *(.note.*) 
    }

    /* Useful symbols */
    __kernel_phys_start = KERNEL_LMA;
    __kernel_phys_end   = LOADADDR(.bss) + SIZEOF(.bss);
    __kernel_virt_end   = ADDR(.bss) + SIZEOF(.bss);
}