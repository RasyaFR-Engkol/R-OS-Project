; Syscall assembly entry: build a CpuContext_T on the kernel stack,
; fill with registers saved by the syscall instruction (and GPRs),
; place pointer to the context in RDI and call the C handler Syscall_Entry.
; NOTE: this stub does NOT perform the user-return sequence (sysret/iret).
; The C handler or higher-level assembly must perform return to user.

BITS 64
GLOBAL Syscall_AsmEntry
EXTERN Arch_SwitchStackAndCall
EXTERN Syscall_Wrapper
EXTERN TSS_GetRsp0

SECTION .text
Syscall_AsmEntry:
    ; Build a CpuContext_T in-place on the current (user) stack so
    ; the iret frame (RIP/CS/RFLAGS/RSP/SS) and GPRs are ready.
    ; CpuContext_T size = 160 bytes
    sub rsp, 160
    mov qword [rsp + 0], r15
    mov qword [rsp + 8], r14
    mov qword [rsp + 16], r13
    mov qword [rsp + 24], r12
    mov qword [rsp + 32], r11
    mov qword [rsp + 40], r10
    mov qword [rsp + 48], r9
    mov qword [rsp + 56], r8

    mov qword [rsp + 64], rdi
    mov qword [rsp + 72], rsi
    mov qword [rsp + 80], rbp
    mov qword [rsp + 88], rbx
    mov qword [rsp + 96], rdx
    mov qword [rsp + 104], rcx
    mov qword [rsp + 112], rax

    ; Software-visible iret frame: RIP (in RCX), CS, RFLAGS (in R11), RSP, SS
    mov qword [rsp + 120], rcx        ; RIP (syscall saved RIP in RCX)
    mov qword [rsp + 128], 0x4B       ; CS (user code selector)
    mov qword [rsp + 136], r11        ; RFLAGS (syscall saved flags in R11)
    lea rax, [rsp + 160]               ; original user RSP = rsp + 160
    mov qword [rsp + 144], rax        ; RSP
    mov qword [rsp + 152], 0x43       ; SS (user data selector)

    ; Call into kernel on the kernel stack: Arch_SwitchStackAndCall(new_sp, Syscall_Wrapper, ctx)
    lea rdx, [rsp]                     ; arg = pointer to CpuContext_T
    call TSS_GetRsp0                    ; returns kernel RSP0 in rax
    mov rdi, rax                        ; new_sp = kernel stack top
    lea rsi, [rel Syscall_Wrapper]     ; target
    jmp Arch_SwitchStackAndCall