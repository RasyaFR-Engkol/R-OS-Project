BITS 64

SECTION .text

GLOBAL Scheduler_IretTrampoline
GLOBAL Context_Restore
EXTERN DumpCpuContext
Scheduler_IretTrampoline:
    ; resume into user/kernel frame saved on stack by CpuContext_T layout
    iretq

    ; VOID ContextRestore(VOID* ContextRSP);
Context_Restore:
    mov rsp, rdi

    ; Restore General Purpose Registers (sesuai urutan push/pop irqstub)
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

    ; Terakhir, iretq untuk pop RIP, CS, RFLAGS, RSP, SS
    iretq
