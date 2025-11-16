#pragma once

#include "rosval.h"
#include <cpu_context.hpp>
VOID Sys_Read(CpuContext_T *CPUContext);
VOID Sys_Write(CpuContext_T *CPUContext);
VOID Sys_Open(CpuContext_T *CPUContext);
VOID Sys_Close(CpuContext_T *CPUContext);