#include "mm.hpp"
#include <rossys.hpp>
// Use project-provided basic types to avoid relying on host libc headers
#include <rosval.h>

// Simple bitmap-backed virtual page allocator.
// This implementation is intentionally small and configurable via the
// constants below. It provides a contiguous-page allocator from a fixed
// virtual pool. Adjust POOL_BASE and POOL_PAGES to match your kernel's
// virtual layout.

extern "C" char __kernel_virt_end; // from linker script

namespace {
    // Number of pages managed by the bitmap (65536 pages = 256 MiB)
    constexpr SIZE_T POOL_PAGES = 65536;
    constexpr SIZE_T BITMAP_BYTES = (POOL_PAGES + 7) / 8;

    static U8 bitmap[BITMAP_BYTES];
    static SIZE_T hint_index = 0; // next place to start searching
    static UPTR g_pool_base = 0;

    inline void bitmap_set(SIZE_T idx){ bitmap[idx >> 3] |= (U8)(1u << (idx & 7)); }
    inline void bitmap_clear(SIZE_T idx){ bitmap[idx >> 3] &= (U8)~(1u << (idx & 7)); }
    inline bool bitmap_test(SIZE_T idx){ return (bitmap[idx >> 3] >> (idx & 7)) & 1u; }
}


namespace PageAlloc{
    void Virtual() {
        // Initialize bitmap: mark all pages free
        for (SIZE_T i = 0; i < BITMAP_BYTES; ++i) bitmap[i] = 0;
        hint_index = 0;

        // Place the kernel heap after the kernel image, aligned to 2 MiB
        UPTR end = (UPTR)&__kernel_virt_end;
        const UPTR TWO_MB = 0x200000ULL;
        g_pool_base = (end + (TWO_MB - 1)) & ~(TWO_MB - 1);
    }
    
    // Fast-ish scan for a run of 'count' free pages starting at 'startIdx'.
    // Scans by bytes where possible to skip full/empty bytes quickly.
    static SIZE_T find_free_run_from(SIZE_T startIdx, SIZE_T count) {
        if (count == 0) return (SIZE_T)(-1);
        if (startIdx >= POOL_PAGES) startIdx = 0;

        SIZE_T idx = startIdx;
        SIZE_T pagesLeft = POOL_PAGES - startIdx;
        for (int pass = 0; pass < 2; ++pass) {
            SIZE_T run = 0;
            SIZE_T i = idx;
            SIZE_T end = startIdx + pagesLeft;
            // byte-based scan
            SIZE_T byteStart = i >> 3;
            SIZE_T bitOff = i & 7;
            for (SIZE_T b = byteStart; (b << 3) < end; ++b) {
                U8 byte = bitmap[b];
                // mask out bits below starting bit in the first byte
                if (b == byteStart && bitOff) {
                    U8 mask = (U8)((1u << bitOff) - 1u);
                    byte |= mask; // treat bits before start as used
                }
                if (byte == 0xFF) {
                    run = 0;
                    continue;
                }
                // fast path: entirely free byte and we still need >= 8
                if (byte == 0x00 && count - run >= 8) {
                    run += 8;
                    if (run >= count) {
                        SIZE_T lastPage = (b << 3) + 7;
                        SIZE_T start = lastPage + 1 - count;
                        return start;
                    }
                    continue;
                }
                // mixed byte; scan bit by bit
                for (U8 bit = (b == byteStart) ? (U8)bitOff : (U8)0; bit < 8; ++bit) {
                    SIZE_T pageIdx = (b << 3) + bit;
                    if (pageIdx >= end) break;
                    if (!bitmap_test(pageIdx)) {
                        ++run;
                        if (run == count) {
                            SIZE_T start = pageIdx + 1 - count;
                            return start;
                        }
                    } else {
                        run = 0;
                    }
                }
            }
            // wrap around once
            idx = 0;
            pagesLeft = startIdx; // the remainder before startIdx
        }
        return (SIZE_T)(-1);
    }

    void* VirtualAllocPages(SIZE_T count) {
        if (count == 0 || count > POOL_PAGES) return nullptr;

        LOCKRFLAGS _irq = Arch::SaveAndDisableInterrupts();
        SIZE_T start = find_free_run_from(hint_index, count);
        if (start == (SIZE_T)(-1)) { Arch::RestoreInterrupts(_irq); return nullptr; }

        SIZE_T end = start + count - 1;
        for (SIZE_T j = start; j <= end; ++j) bitmap_set(j);
        hint_index = (end + 1) % POOL_PAGES;
        UPTR addr = g_pool_base + (UPTR)start * (UPTR)PAGE_SIZE;
        Arch::RestoreInterrupts(_irq);
        return (void*)addr;
    }

    void VirtualFreePages(void* addr, SIZE_T count) {
        if (addr == nullptr || count == 0) return;
        UPTR a = (UPTR)addr;
        if (a < g_pool_base) return; // out of pool
        SIZE_T idx = (SIZE_T)((a - g_pool_base) / PAGE_SIZE);
        if (idx + count > POOL_PAGES) return; // out of range
        LOCKRFLAGS _irq = Arch::SaveAndDisableInterrupts();
        for (SIZE_T i = 0; i < count; ++i) bitmap_clear(idx + i);
        if (idx < hint_index) hint_index = idx; // try to allocate from earlier free space next time
        Arch::RestoreInterrupts(_irq);
    }
}