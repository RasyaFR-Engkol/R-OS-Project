; Syscall assembly entry: build a CpuContext_T on the kernel stack,
; fill with registers saved by the syscall instruction (and GPRs),
; place pointer to the context in RDI and call the C handler Syscall_Entry.
; NOTE: this stub does NOT perform the user-return sequence (sysret/iret).
; The C handler or higher-level assembly must perform return to user.

BITS 64
GLOBAL Syscall_AsmEntry
EXTERN Syscall_Entry

SECTION .text
Syscall_AsmEntry:
    sysret