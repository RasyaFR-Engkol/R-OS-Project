#pragma once

#include <rosval.h>
#include <cpu_context.hpp>
#include <ros_linux/mmprotocol.hpp>

VOID Sys_Brk(CpuContext_T *CPUContext);
VOID Sys_MMap(CpuContext_T *CPUContext);
VOID Sys_ShmOpen(CpuContext_T *Ctx);
VOID Sys_Munmap(CpuContext_T *CPUContext);
VOID Sys_Mprotect(CpuContext_T *ctx);