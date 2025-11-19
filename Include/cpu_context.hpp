#pragma once

#include <rosval.h>

/*
 * CpuContext_T
 * Full CPU context structure used for saving/restoring a thread/process
 * context across kernel/user transitions and context switches.
 *
 * Layout intentionally groups general-purpose registers first, then
 * the software-visible return frame (RIP/CS/RFLAGS/RSP/SS) and control
 * registers so it can be used directly by an iretq trampoline or
 * context switcher implemented in assembly.
 */
typedef struct CpuContext_T {
	/* Callee-saved general purpose registers (and extra regs) */
	U64 r15;
	U64 r14;
	U64 r13;
	U64 r12;
	U64 r11;
	U64 r10;
	U64 r9;
	U64 r8;

	/* Common registers (caller-saved / general) */
	U64 rdi;
	U64 rsi;
	U64 rbp;
	U64 rbx;
	U64 rdx;
	U64 rcx;
	U64 rax;

	/* Software-visible frame for returning to userspace (used by iretq):
	 * on the stack an iretq sees: RIP, CS, RFLAGS, RSP, SS
	 */
	U64 rip;
	U64 cs;
	U64 rflags;
	U64 rsp;
	U64 ss;
} CpuContext_T;

