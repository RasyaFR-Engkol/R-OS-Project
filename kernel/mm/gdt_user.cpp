/* Add userland (ring3) code and data descriptors into the current GDT.
 * We choose safe indices that avoid the boot-provided kernel entries and
 * the TSS descriptor. Boot layout (see boot.asm) is:
 * 0: null
 * 1: kernel code
 * 2: kernel data
 * 3-4: TSS (installed by TSS::Init)
 * We'll place user code/data at indices 5 and 6.
 */

#include "gdt_user.hpp"
#include <rosval.h>
#include <string.hpp>
#include "../Include/serial.hpp"

using namespace Serial;

namespace Paging {

    // Place user descriptors at GDT indices 8 and 9 to avoid clobbering
    // the kernel's entries (the TSS uses indices 6..7). Choose indices
    // beyond that so we don't overlap system descriptors.
    static constexpr unsigned USER_DATA_INDEX = 8;
    static constexpr unsigned USER_CODE_INDEX = 9;

    // Build a standard 8-byte GDT descriptor.
    // `access` holds the type/S/DPL/P bits (e.g. 0xFA for user code), and
    // `flags` uses the low 4 bits for (G | D/B | L | AVL). The base/limit are
    // ignored in long mode code segments but kept for completeness.
    static inline U64 build_seg_descriptor(U32 base, U32 limit, U8 access, U8 flags) {
        U64 desc = 0;
        desc |= (limit & 0xFFFFULL);                       // limit[0:15]
        desc |= ((U64)(base & 0xFFFFFFULL)) << 16;         // base[0:23]
        desc |= ((U64)access) << 40;                       // type/S/DPL/P
        desc |= ((U64)((limit >> 16) & 0xF)) << 48;        // limit[16:19]
        desc |= ((U64)(flags & 0xF)) << 52;                // AVL/L/D/B/G
        desc |= ((U64)((base >> 24) & 0xFF)) << 56;        // base[24:31]
        return desc;
    }

    static void write_single_descriptor(U64 entry, unsigned index) {
        struct GDTR { U16 limit; U64 base; } __attribute__((packed));
        GDTR gdtr;
        asm volatile("sgdt %0" : "=m"(gdtr));

        U64 *gdt = (U64*)gdtr.base;
        gdt[index] = entry;

        // Ensure GDTR.limit covers this index
        U16 needed_limit = (U16)(((index + 1) * 8) - 1);
        if (gdtr.limit < needed_limit) {
            GDTR newgdtr { needed_limit, gdtr.base };
            asm volatile("lgdt %0" :: "m"(newgdtr));
        }
    }

    static void DumpGDTEntries() {
        struct GDTR { U16 limit; U64 base; } __attribute__((packed));
        GDTR gdtr;
        asm volatile("sgdt %0" : "=m"(gdtr));

        U64 entryCount = ((U64)gdtr.limit + 1ULL) / 8ULL;
        U64 *gdt = reinterpret_cast<U64*>(gdtr.base);

        Serial::Printf("[ROS] GDT dump: base=%p entries=%llu\n", (void*)gdtr.base,
                       (unsigned long long)entryCount);
        for (U64 i = 0; i < entryCount; ++i) {
            Serial::Printf("[ROS]  GDT[%02llu] = 0x%016llx\n",
                           (unsigned long long)i,
                           (unsigned long long)gdt[i]);
        }
    }

    void AddUserGDTEntries() {
        // Access bytes: set Present, DPL=3, S=1, Type bits (code/data).
        // For code: executable + readable -> type 0xA. For data: writable -> 0x2.
        // Kernel code used 0x9A in boot (DPL=0). To get DPL=3, set bits 6..5 = 11b -> add 0x60.
        const U8 KERNEL_CODE_ACCESS = 0x9A; // as in boot.asm
        const U8 KERNEL_DATA_ACCESS = 0x92;
        const U8 DPL3_MASK = 0x60; // bits 5-6 = 11

        U8 user_code_access = KERNEL_CODE_ACCESS | DPL3_MASK; // 0xFA
        U8 user_data_access = KERNEL_DATA_ACCESS | DPL3_MASK; // 0xF2

        // Flags: code segment must have L=1 and D/B=0 for 64-bit code.
        // Data segment should be a 64-bit-compatible data descriptor (D/B=0).
        const U8 CODE_FLAGS = 0xA; // G=1, D/B=0, L=1, AVL=0
        const U8 DATA_FLAGS = 0x8; // G=1, D/B=0, L=0, AVL=0

        U64 code_desc = build_seg_descriptor(0, 0, user_code_access, CODE_FLAGS);
        U64 data_desc = build_seg_descriptor(0, 0, user_data_access, DATA_FLAGS);

        // Write user code and data descriptors at the chosen indices.
        write_single_descriptor(code_desc, USER_CODE_INDEX);
        write_single_descriptor(data_desc, USER_DATA_INDEX);

        Serial::Printf("[ROS] Added user GDT entries: code_idx=%u data_idx=%u\n",
            (unsigned)USER_CODE_INDEX, (unsigned)USER_DATA_INDEX);
        DumpGDTEntries();
    }

} // namespace Paging
