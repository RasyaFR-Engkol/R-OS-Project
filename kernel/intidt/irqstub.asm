BITS 64

section .text

; Common C handler: void IrqDispatch(unsigned long irq)
EXTERN IrqDispatch
EXTERN IrqDispatchWithRawStack

%macro MAKE_IRQ 1
GLOBAL IrqStub_%1
IrqStub_%1:
    ; Save registers (do NOT push/pop rsp)
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

    ; Arg1: irq number in rdi per SysV ABI
    ; Arg2: pointer to saved registers on stack (rsp) in rsi
    mov rdi, %1
    mov rsi, rsp
    call IrqDispatchWithRawStack

    ; Restore registers
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
    iretq
%endmacro

; Generate 208 IRQ stubs: IrqStub_0 .. IrqStub_207
%assign i 0
%rep 208
    MAKE_IRQ i
%assign i i+1
%endrep

section .rodata align=8
GLOBAL IrqStub_Table
IrqStub_Table:
%assign i 0
%rep 208
    dq IrqStub_%+i
%assign i i+1
%endrep