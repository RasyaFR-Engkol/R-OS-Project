#pragma once
#include <rosval.h>

struct KernelSymbol{
    UPTR Value;
    CONSTANT CHAR8* Name;
};

extern "C" KernelSymbol __ksymtab_start[];
extern "C" KernelSymbol __ksymtab_end[];

#define EXPORT_SYMBOL(sym) \
    extern "C" const char __kstrtab_##sym[] __attribute__((section("__ksymtab_strings"), aligned(1))) = #sym; \
    extern "C" const KernelSymbol __ksymtab_##sym __attribute__((section("__ksymtab"), used, aligned(8))) = { \
        (uintptr_t)&sym, \
        __kstrtab_##sym \
    };
    