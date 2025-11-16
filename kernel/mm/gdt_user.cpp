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

    static constexpr unsigned USER_CODE_INDEX = 5;
    static constexpr unsigned USER_DATA_INDEX = 6;

    // Build a normal 8-byte GDT entry for code/data segments. In long mode
    // the base/limit fields are ignored for code segments; we set them to 0.
    static inline U64 build_seg_descriptor(U8 access, U8 flags) {
        U64 desc = 0;
        // limit low (16) = 0
        // base low/mid/high = 0
        desc |= ((U64)access) << 40;
        desc |= ((U64)flags) << 52;
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

    void AddUserGDTEntries() {
        // Access bytes: set Present, DPL=3, S=1, Type bits (code/data).
        // For code: executable + readable -> type 0xA. For data: writable -> 0x2.
        // Kernel code used 0x9A in boot (DPL=0). To get DPL=3, set bits 6..5 = 11b -> add 0x60.
        const U8 KERNEL_CODE_ACCESS = 0x9A; // as in boot.asm
        const U8 KERNEL_DATA_ACCESS = 0x92;
        const U8 DPL3_MASK = 0x60; // bits 5-6 = 11

        U8 user_code_access = KERNEL_CODE_ACCESS | DPL3_MASK; // 0xFA
        U8 user_data_access = KERNEL_DATA_ACCESS | DPL3_MASK; // 0xF2

        // Flags: keep same convention as boot (0x20 for long mode code, 0x00 for data)
        const U8 CODE_FLAGS = 0x20;
        const U8 DATA_FLAGS = 0x00;

        U64 code_desc = build_seg_descriptor(user_code_access, CODE_FLAGS);
        U64 data_desc = build_seg_descriptor(user_data_access, DATA_FLAGS);

        write_single_descriptor(code_desc, USER_CODE_INDEX);
        write_single_descriptor(data_desc, USER_DATA_INDEX);

        Serial::Printf("[ROS] Added user GDT entries: code_idx=%u data_idx=%u\n",
            (unsigned)USER_CODE_INDEX, (unsigned)USER_DATA_INDEX);
    }

} // namespace Paging
