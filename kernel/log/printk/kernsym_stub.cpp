/*
 * Kernel symbol lookup stub.
 *
 * Projects that want symbolicated stack traces can provide their own
 * implementation of LookupKernelSymbol(addr, &sym_addr) (link-time
 * replacement). This file provides a weak fallback that returns NULL.
 */

#include <rosval.h>

extern "C" const CHAR8* LookupKernelSymbol(UPTR addr, UPTR* sym_addr) __attribute__((weak));

const CHAR8* LookupKernelSymbol(UPTR addr, UPTR* sym_addr) {
    if (sym_addr) *sym_addr = 0;
    return nullptr;
}
