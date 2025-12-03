#pragma once

#include "rosval.h"
#include <cpu_context.hpp>

typedef long time_t;

struct timespec {
    time_t tv_sec;  /* Seconds */
    long   tv_nsec; /* Nanoseconds */
};

VOID Sys_Exit(CpuContext_T *CPUContext);
VOID Sys_Fork(CpuContext_T *CPUContext);
VOID Sys_Execve(CpuContext_T *CPUContext);
VOID Sys_Wait(CpuContext_T *CPUContext);
VOID Sys_GetPID(CpuContext_T *CPUContext);
VOID Sys_GetPGID(CpuContext_T *CPUContext);
VOID Sys_SetPGID(CpuContext_T *CPUContext);
VOID Sys_Signal(CpuContext_T *CPUContext);
VOID Sys_RtSigAction(CpuContext_T *CPUContext);
VOID SysSleepMS(CpuContext_T *CPUContext);