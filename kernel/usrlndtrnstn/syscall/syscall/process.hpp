#pragma once

#include "rosval.h"
#include <cpu_context.hpp>

VOID Sys_Exit(CpuContext_T *CPUContext);
VOID Sys_Fork(CpuContext_T *CPUContext);
VOID Sys_Execve(CpuContext_T *CPUContext);
VOID Sys_Wait(CpuContext_T *CPUContext);
VOID Sys_GetPID(CpuContext_T *CPUContext);
VOID Sys_GetPGID(CpuContext_T *CPUContext);
VOID Sys_SetPGID(CpuContext_T *CPUContext);