#pragma once
#include <rosval.h>

struct KernelSymbol{
    int32_t value_offset; // Jarak dari member ini ke alamat fungsi/variabel
    int32_t name_offset;  // Jarak dari member ini ke alamat string nama
};

extern "C" KernelSymbol __ksymtab_start[];
extern "C" KernelSymbol __ksymtab_end[];

#define EXPORT_SYMBOL(sym) \
    extern "C" const char __kstrtab_##sym[] __attribute__((section("__ksymtab_strings"), aligned(1))) = #sym; \
    asm( \
        ".section \"__ksymtab\", \"a\"\n" \
        ".align 4\n" \
        "__ksymtab_" #sym ":\n" \
        ".long " #sym " - .\n"         /* Offset value */ \
        ".long __kstrtab_" #sym " - .\n" /* Offset name */ \
        ".previous\n" \
    );

static inline uintptr_t resolve_symbol_address(const int32_t* offset_ptr) {
    return (uintptr_t)offset_ptr + *offset_ptr;
}