#pragma once

#include "rosval.h"
#include <cpu_context.hpp>

typedef long time_t;

struct timespec {
    time_t tv_sec;  /* Seconds */
    long   tv_nsec; /* Nanoseconds */
};

struct kernel_pollfd {
    int   fd;         // File descriptor
    short events;     // Events yang diminta (misal: POLLIN)
    short revents;    // Events yang terjadi (return value)
};

// Konstanta bitmask poll (Standar)
#define POLLIN      0x0001
#define POLLPRI     0x0002
#define POLLOUT     0x0004
#define POLLERR     0x0008
#define POLLHUP     0x0010
#define POLLNVAL    0x0020

VOID Sys_Exit(CpuContext_T *CPUContext);
VOID Sys_Fork(CpuContext_T *CPUContext);
VOID Sys_Execve(CpuContext_T *CPUContext);
VOID Sys_Wait(CpuContext_T *CPUContext);
VOID Sys_GetPID(CpuContext_T *CPUContext);
VOID Sys_GetPGID(CpuContext_T *CPUContext);
VOID Sys_SetPGID(CpuContext_T *CPUContext);
VOID Sys_Signal(CpuContext_T *CPUContext);
VOID Sys_RtSigAction(CpuContext_T *CPUContext);
VOID Sys_SleepNs(CpuContext_T *CPUContext);
VOID SysSleepMS(CpuContext_T *CPUContext);
VOID Sys_Poll(CpuContext_T *CPUContext);
VOID Sys_Yield(CpuContext_T *CPUContext);
VOID Sys_SetAppPerm(CpuContext_T *CPUContext);
VOID Sys_GetClockTime(CpuContext_T *CPUContext);