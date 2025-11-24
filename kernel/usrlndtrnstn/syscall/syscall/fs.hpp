#pragma once

#include "rosval.h"
#include <cpu_context.hpp>
VOID Sys_Read(CpuContext_T *CPUContext);
VOID Sys_Write(CpuContext_T *CPUContext);
VOID Sys_Open(CpuContext_T *CPUContext);
VOID Sys_Close(CpuContext_T *CPUContext);
VOID Sys_Dup2(CpuContext_T *CPUContext);
VOID Sys_Getdents64(CpuContext_T *CPUContext);
VOID Sys_Chdir(CpuContext_T *CPUContext);
VOID Sys_GetCWD(CpuContext_T *CPUContext);
VOID Sys_Pipe(CpuContext_T *CPUContext);
VOID Sys_Ioctl(CpuContext_T *CPUContext);