#pragma once
#include <rosval.h>

namespace ModuleManager {
    int LoadModuleAndRun(VOID* FileBuffer, VOID *PrivateData);
    UPTR FindKernelSymbol(const char* name);
}