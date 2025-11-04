; Simple stack switch trampoline for SysV x86_64 ABI
BITS 64

SECTION .text
GLOBAL Arch_SwitchStackAndCall
; void Arch_SwitchStackAndCall(void* new_sp, void (*target)(void*), void* arg);
; rdi=new_sp, rsi=target, rdx=arg
Arch_SwitchStackAndCall:
    mov     rsp, rdi        ; switch to new stack
    ; Preserve RBP to honor SysV ABI (callee-saved)
    mov     rdi, rdx        ; move arg into rdi per ABI
    jmp     rsi             ; tail call target(arg) on new stack

GLOBAL Arch_SwitchStackAndResume
; void Arch_SwitchStackAndResume(void* new_sp, void* resume_ip)
; rdi=new_sp, rsi=resume_ip
Arch_SwitchStackAndResume:
    mov     rsp, rdi        ; switch to new stack
    ; Preserve RBP to honor SysV ABI (callee-saved)
    push    rsi             ; push resume address
    ret                     ; jump to resume_ip with new stack

GLOBAL Arch_SwitchStackResume2
; void Arch_SwitchStackResume2(void* new_sp, void* first_ip, void* second_ip)
; rdi=new_sp, rsi=first_ip, rdx=second_ip
Arch_SwitchStackResume2:
    mov     rsp, rdi        ; switch to new stack
    ; Preserve RBP to honor SysV ABI (callee-saved)
    push    rdx             ; second return address (where first_ip will return)
    push    rsi             ; first resume address
    ret                     ; jump to first_ip; when it returns, it goes to second_ip
