BITS 64

SECTION .text
ALIGN 16
GLOBAL IsrStub_PageFault
GLOBAL IsrStub_GPFault
EXTERN PageFaultHandler
EXTERN GPFaultHandler

IsrStub_PageFault:
    ; Save Registers
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; 1. Ambil Argumen 1 (Faulting Address) dari CR2
    mov rdi, cr2

    ; 2. Ambil Argumen 2 (Error Code) dari Stack
    ;    (Stack sekarang berisi 15 * 8 = 120 bytes dari register kita)
    ;    Jadi, Error Code ada di RSP + 120
    mov rsi, [rsp + 120]

    ; Panggil handler C++
    call PageFaultHandler

    ; Restore Registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx 
    pop rdx
    pop rcx
    pop rax

    ; 3. Bersihkan Error Code dari Stack (SANGAT PENTING!)
    add rsp, 8

    ; Return from interrupt
    iretq

ALIGN 16
IsrStub_GPFault:
    ; Save all registers
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; 1. Ambil Error Code dari Stack
    ;    (Stack sekarang berisi 15 * 8 = 120 bytes dari register kita)
    ;    Jadi, Error Code ada di RSP + 120
    mov rdi, [rsp + 120]

    ; 2. Kirim stack pointer (berisi saved registers) sebagai argumen kedua
    mov rsi, rsp

    ; Panggil handler C++
    call GPFaultHandler

    ; Restore Registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx 
    pop rdx
    pop rcx
    pop rax

    ; 3. Bersihkan Error Code dari Stack
    add rsp, 8

    ; Return from interrupt
    iretq