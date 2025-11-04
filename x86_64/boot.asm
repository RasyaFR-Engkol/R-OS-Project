; Minimal Multiboot2/UEFI capable 64-bit bootstrap for ROS

BITS 32

SECTION .multiboot2_header
ALIGN 8

multiboot2_header:
	dd 0xE85250D6                ; magic
	dd 0                        ; architecture (x86)
	dd header_end - multiboot2_header
	dd -(0xE85250D6 + 0 + (header_end - multiboot2_header))

ALIGN 8
header_tag_entry:
	dw 3                        ; entry address (BIOS)
	dw 0
	dd header_tag_entry_end - header_tag_entry
	dq _start
header_tag_entry_end:

ALIGN 8
header_tag_entry_efi64:
	dw 9                        ; entry address for EFI64
	dw 0
	dd header_tag_entry_efi64_end - header_tag_entry_efi64
	dq _start
header_tag_entry_efi64_end:

ALIGN 8
header_tag_framebuffer:
	dw 5                        ; framebuffer request
	dw 0                        ; flags
	dd header_tag_framebuffer_end - header_tag_framebuffer
	dd 1024                     ; width
	dd 768                      ; height
	dd 0                        ; depth (bpp) 0 = no preference
header_tag_framebuffer_end:

ALIGN 8
header_tag_end:
	dw 0
	dw 0
	dd 8
header_end:


SECTION .boottext
ALIGN 16

GLOBAL _start
EXTERN KernelEntryPoint
EXTERN __kernel_phys_start
EXTERN __kernel_phys_end

CODE_SEG EQU 0x08
DATA_SEG EQU 0x10
KERNEL_VMA EQU 0xFFFFFFFF80000000

%define PTE_P   0x1
%define PTE_W   0x2
%define PTE_PS  0x80
%define FLAGS_RW    (PTE_P | PTE_W)
%define FLAGS_LARGE (PTE_P | PTE_W | PTE_PS)

_start:
	cli
	mov esp, stack_top
	mov [mb_info_ptr], ebx       ; stash Multiboot2 info pointer
	mov dword [mb_info_ptr + 4], 0

	lgdt [gdt_descriptor]

	; Build minimal 4-level paging structure with 1 GiB identity mapping
	mov eax, pdpt_table
	or eax, 0x3                  ; present + writable
	mov dword [pml4_table], eax
	mov dword [pml4_table + 4], 0

	mov eax, pd_table
	or eax, 0x3
	mov dword [pdpt_table], eax
	mov dword [pdpt_table + 4], 0

	; Fill 512 entries: 512 * 2MiB = 1GiB identity map
	mov ecx, 512
	xor edx, edx                  ; i = 0
.fill_id:
	mov eax, edx
	shl eax, 21                  ; phys = i * 2MiB
	or eax, 0x83                 ; present + writable + PS
	mov [pd_table + edx*8], eax
	mov dword [pd_table + edx*8 + 4], 0
	inc edx
	loop .fill_id

	mov eax, cr4
	or eax, 1 << 5               ; CR4.PAE
	mov cr4, eax

	mov eax, pml4_table
	mov cr3, eax

	mov ecx, 0xC0000080          ; IA32_EFER
	rdmsr
	or eax, 1 << 8               ; enable long mode
	wrmsr

	mov eax, cr0
	or eax, (1 << 31) | 1        ; enable paging + ensure PE is set
	mov cr0, eax

	jmp CODE_SEG:long_mode_entry


BITS 64

long_mode_entry:
	mov ax, DATA_SEG
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov fs, ax
	mov gs, ax

	lea rsp, [rel stack_top]

	; Map the higher-half kernel at KERNEL_VMA using 2MiB pages
	; Set PML4 entry for KERNEL_VMA
	mov rax, KERNEL_VMA
	mov rbx, rax
	shr rax, 39
	and rax, 0x1FF
	lea rcx, [rel pml4_table]
	lea rdx, [rel kernel_pdpt_table]
	or rdx, FLAGS_RW
	mov [rcx + rax*8], rdx

	; Set PDPT entry for KERNEL_VMA
	mov rax, rbx
	shr rax, 30
	and rax, 0x1FF
	lea rcx, [rel kernel_pdpt_table]
	lea rdx, [rel kernel_pd_table]
	or rdx, FLAGS_RW
	mov [rcx + rax*8], rdx

	; Zero the kernel PD table
	lea rsi, [rel kernel_pd_table]
	mov rcx, 512
	xor rdx, rdx
.zero_kpd:
	mov [rsi], rdx
	add rsi, 8
	loop .zero_kpd

	; PD index base within the 1GiB region
	mov rax, KERNEL_VMA
	shr rax, 21
	and rax, 0x1FF

	; Calculate number of 2MiB pages covering the kernel image
	mov r8, __kernel_phys_end
	sub r8, __kernel_phys_start
	add r8, 0x1FFFFF
	shr r8, 21

	; Physical start
	mov r9, __kernel_phys_start
	lea rdi, [rel kernel_pd_table]
	xor rbx, rbx                   ; i = 0
.map_kpd:
	test r8, r8
	jz .map_done
	mov r10, rax                   ; idx = base + i
	add r10, rbx
	cmp r10, 512
	jae .map_done                  ; stop if would overflow
	mov r11, r9
	or r11, FLAGS_LARGE
	mov [rdi + r10*8], r11
	add r9, 0x200000               ; next 2MiB
	inc rbx
	dec r8
	jmp .map_kpd
.map_done:

	; Jump to higher-half KernelEntryPoint with rdi=mb_info_ptr
	mov rdi, [rel mb_info_ptr]
	mov rax, KernelEntryPoint
	jmp rax

.hang:
	hlt
	jmp .hang


SECTION .bootdata

ALIGN 8
mb_info_ptr: dq 0


SECTION .bootbss

ALIGN 4096
GLOBAL pml4_table
GLOBAL pdpt_table
GLOBAL pd_table
GLOBAL kernel_pdpt_table
GLOBAL kernel_pd_table
pml4_table:   resq 512
pdpt_table:   resq 512
pd_table:     resq 512
kernel_pdpt_table: resq 512
kernel_pd_table:   resq 512

ALIGN 16
stack_bottom: resb 0x4000
stack_top:


SECTION .bootrodata

ALIGN 8
gdt_table:
	dq 0
	dw 0, 0
	db 0
	db 0x9A                     ; code access
	db 0x20                     ; long mode flag
	db 0
	dw 0, 0
	db 0
	db 0x92                     ; data access
	db 0x00
	db 0

gdt_end:

gdt_descriptor:
	dw gdt_end - gdt_table - 1
	dd gdt_table
