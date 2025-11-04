/* Minimal 64-bit TSS implementation
 * - Builds a 64-bit TSS descriptor in the GDT (two 8-byte entries)
 * - Loads the TR (ltr) with the selector
 * - Provides runtime update for rsp0
 */

#include "tss.hpp"
#include <serial.hpp>
#include <rosval.h>

using namespace Serial;

struct __attribute__((packed)) tss_struct {
    U32 reserved0;
    U64 rsp0;
    U64 rsp1;
    U64 rsp2;
    U64 reserved1;
    U64 ist1;
    U64 ist2;
    U64 ist3;
    U64 ist4;
    U64 ist5;
    U64 ist6;
    U64 ist7;
    U64 reserved2;
    U16 reserved3;
    U16 iomap_base;
};

static tss_struct g_tss __attribute__((aligned(16)));

// GDT selector index to place TSS (pick index 3 => selector = 3*8 = 0x18)
static constexpr unsigned TSS_GDT_INDEX = 3;

// Build 16-byte TSS descriptor fields (low 8 bytes and high 8 bytes)
static void build_tss_descriptor(U64 base, U32 limit, U64 &out_low, U64 &out_high) {
    U64 b = base;
    U64 l = limit;
    U64 low = 0;
    U64 high = 0;

    low  = (l & 0xFFFFULL);
    low |= ( (b & 0xFFFFULL) << 16 );
    low |= ( ((b >> 16) & 0xFFULL) << 32 );
    // Type 0x9 = 64-bit available TSS (0x89 = present + type)
    low |= ( (U64)0x89ULL << 40 );
    low |= ( ((l >> 16) & 0xFULL) << 48 );
    low |= ( ((b >> 24) & 0xFFULL) << 56 );

    high = (b >> 32) & 0xFFFFFFFFULL;

    out_low = low;
    out_high = high;
}

static void write_descriptor_to_gdt(U64 low, U64 high, unsigned index) {
    struct GDTR { U16 limit; U64 base; } __attribute__((packed));
    GDTR gdtr;
    asm volatile("sgdt %0" : "=m"(gdtr));

    U64 *gdt = (U64*)gdtr.base;
    // place the two 8-byte entries at index and index+1
    gdt[index] = low;
    gdt[index + 1] = high;

    // Ensure the GDTR.limit covers the newly added descriptor entries.
    U16 needed_limit = (U16)(((index + 2) * 8) - 1);
    if (gdtr.limit < needed_limit) {
        GDTR newgdtr { needed_limit, gdtr.base };
        asm volatile("lgdt %0" :: "m"(newgdtr));
    }
}

static void load_tr_selector(U16 selector) {
    asm volatile("ltr %0" : : "r"(selector) : "memory");
}

namespace TSS {

    void Init(UPTR rsp0_top) {
        // zero the TSS
        for (size_t i = 0; i < sizeof(g_tss); ++i) ((U8*)&g_tss)[i] = 0;
        g_tss.iomap_base = sizeof(g_tss);

        // set RSP0
        g_tss.rsp0 = (U64)rsp0_top;

        // Provide at least IST1 as a safety (optional)
        // IST stacks can be set later via SetRsp0 or dedicated setter

        // Build descriptor and install into GDT
        U64 low, high;
        build_tss_descriptor((U64)&g_tss, (U32)(sizeof(g_tss) - 1), low, high);
        write_descriptor_to_gdt(low, high, TSS_GDT_INDEX);

        U16 selector = (U16)(TSS_GDT_INDEX * 8);
        load_tr_selector(selector);

        Serial::Printf("[ROS] TSS initialized: rsp0=%p selector=0x%04x\n", (void*)g_tss.rsp0, selector);
    }

    void SetRsp0(UPTR rsp0_top) {
        g_tss.rsp0 = (U64)rsp0_top;
    }

} // namespace TSS
