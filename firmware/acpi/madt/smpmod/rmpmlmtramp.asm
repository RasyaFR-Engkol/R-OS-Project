; =====================================
; R-OS Firmware
; ACPI MADT SMP Module
; RMPMLMTRAMP.ASM
; =====================================

; -------------------------------------
; 16 Bit Real Mode Long Mode Trampoline
; -------------------------------------
; This code is used to switch from 16-bit real mode to 64-bit long mode
; for application processors (APs) during SMP initialization.
; It sets up the necessary environment and jumps to the long mode entry point.

section .text
BITS 16
ORG 0x8000

global trampoline_start
trampoline_start:
cli
; Load GDT using 6-byte descriptor (limit:16, base:32) valid in real/protected mode
lgdt [gdt_ptr16]

mov eax, cr0
or eax, 1
mov cr0, eax
jmp 0x08:tahap_2_protected_mode

BITS 32
tahap_2_protected_mode:
mov ax, 0x10
mov ds, ax
mov es, ax
mov ss, ax

; Enable PAE before loading CR3; then load PML4 base (must be <4GiB)
mov eax, cr4
or  eax, (1 << 5)        ; CR4.PAE
mov cr4, eax

; Load PML4 base for long mode paging
mov eax, [pml4_addr]
mov cr3, eax

; Enable Long Mode (LME) in EFER
mov ecx, 0xC0000080      ; IA32_EFER
rdmsr
or  eax, (1 << 8)        ; LME
wrmsr

; Enable paging (CR0.PG). PE is already set.
mov eax, cr0
or  eax, (1 << 31)
mov cr0, eax

; Far jump to 64-bit code segment to activate Long Mode
jmp 0x18:tahap_3_long_mode

BITS 64
tahap_3_long_mode:
; In long mode, reload data segments with a valid present data selector (0x10)
; Avoid reloading GDTR here unless you provide a valid 10-byte pointer.
mov ax, 0x10
mov ds, ax
mov es, ax
mov ss, ax

mov rsp, [ap_stack_addr]
mov rdi, [apic_id]
mov rax, [ap_main_func]
call rax

done:
    cli
    hlt
    jmp done

; Data Structures
align 8
; 6-byte GDT pointer for real/protected mode LGDT (limit:16, base:32)
gdt_ptr16:
  dw gdt_end - gdt_start - 1
  dd gdt_start

; PML4 physical base (must be < 4GiB because we load CR3 in 32-bit mode)
pml4_addr: dd 0x00000000 ; To be filled with actual PML4 address
; Optional 10-byte GDT pointer for 64-bit LGDT if desired (not used currently)
gdt_kernel_pointer: dq 0
ap_stack_addr: dq 0x00000000 ; To be filled with actual stack address for AP
ap_main_func: dq 0x00000000 ; To be filled with actual AP main function address
apic_id: dq 0x00000000 ; To be filled with actual APIC

; GDT sementara (minimal)
align 8
gdt_start:
  dq 0x0000000000000000        ; Null descriptor
gdt_code_32:
  dq 0x00CF9A000000FFFF        ; Base=0, Limit=4GiB-1, 32-bit code
gdt_data_32:
  dq 0x00CF92000000FFFF        ; Base=0, Limit=4GiB-1, 32-bit data
gdt_code_64:
  dq 0x00AF9A000000FFFF        ; Base=0, Limit=4GiB-1, 64-bit code (L-bit set)
gdt_end:

trampoline_end:

align 4
tramp_meta_signature:   dd 0x54524D50        ; 'TRMP'
tramp_meta_version:     dd 1
tramp_meta_pml4_off:    dd pml4_addr - trampoline_start
tramp_meta_gdtptr_off:  dd gdt_kernel_pointer - trampoline_start
tramp_meta_stack_off:   dd ap_stack_addr - trampoline_start
tramp_meta_entry_off:   dd ap_main_func - trampoline_start
tramp_meta_apic_off:    dd apic_id - trampoline_start
tramp_meta_size:        dd trampoline_end - trampoline_start