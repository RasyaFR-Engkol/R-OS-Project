#include <rosval.h>
#include "mm.hpp"
#include <string.hpp>
#include <bootinfo.h>
#include "../kmalloc/kmalloc.hpp"
#include <serial.hpp>
#include <rossys.hpp>
#include <logging.hpp>

// DMA buffer allocator backed by a dedicated pool using a bitmap.
// - Reserves a contiguous range of physical pages for DMA
// - Reserves a contiguous range of virtual pages and maps them to the pool
// - Uses a bitmap to sub-allocate contiguous runs of pages for callers
// - Places mfence barriers around allocation/free for ordering

namespace {
    // Default pool size (pages). We'll try this and fall back smaller if needed.
    constexpr SIZE_T DMA_POOL_PAGES_DEFAULT = 4096; // 16 MiB
    // Upper cap for DMA pool (pages) to avoid starving system; default 256 MiB
    constexpr SIZE_T DMA_POOL_PAGES_MAX = 65536; // 256 MiB

    // Pool state
    static UPTR   g_poolPhys = 0;
    static UPTR   g_poolVirt = 0;
    static SIZE_T g_poolPages = 0;
    static U8*    g_bitmap = nullptr; // size = (g_poolPages + 7) / 8

    inline void mfence() {
        asm volatile("mfence" ::: "memory");
    }

    inline void bmp_set(SIZE_T idx)   { g_bitmap[idx >> 3] |=  (U8)(1u << (idx & 7)); }
    inline void bmp_clear(SIZE_T idx) { g_bitmap[idx >> 3] &= (U8)~(1u << (idx & 7)); }
    inline bool bmp_test(SIZE_T idx)  { return (g_bitmap[idx >> 3] >> (idx & 7)) & 1u; }
}

namespace PageAlloc {
namespace DMAAlloc {

    void InitializeDMA() {
        if (g_poolPages != 0) return; // already initialized

        // Choose desired pool size based on available RAM (E820) when possible.
        SIZE_T want = DMA_POOL_PAGES_DEFAULT;
        if (const BootInfo* bi = BootInfoGet()) {
            if (bi->has_memmap) {
                // Find the largest RAM (type==1) region reported by E820.
                U64 largest_bytes = 0;
                for (U32 i = 0; i < bi->memmap.count; ++i) {
                    const BootMemRegion &r = bi->memmap.regions[i];
                    if (r.type == 1) {
                        if (r.length > largest_bytes) largest_bytes = r.length;
                    }
                }
                if (largest_bytes >= PAGE_SIZE) {
                    SIZE_T largest_pages = (SIZE_T)(largest_bytes / PAGE_SIZE);
                    // Aim for up to 1/8th of the largest RAM region but cap to MAX.
                    SIZE_T candidate = largest_pages / 8;
                    if (candidate > want) {
                        if (candidate > DMA_POOL_PAGES_MAX) candidate = DMA_POOL_PAGES_MAX;
                        want = candidate;
                        Printk::Write(Printk::Level::LOG_INFO, "[DMA] Pool size adjusted to %u pages (1/8th of largest RAM region)\n", (unsigned)want);
                    }
                }
            }
        }
        UPTR phys = 0;
        while (want >= 64) {
            phys = PageAlloc::PhysicalAllocPages(want);
            if (phys) break;
            want /= 2; // fallback smaller
        }
        if (!phys) {
            Serial::Write("[DMA] Failed to reserve physical pool\n");
            return;
        }

        void* vbase = PageAlloc::VirtualAllocPages(want);
        if (!vbase) {
            PageAlloc::PhysicalFreePages(phys, want);
            Serial::Write("[DMA] Failed to reserve virtual range\n");
            return;
        }

        if (!PageAlloc::MapPages(KernelPML4, phys, (UPTR)vbase, want, PAGE_PRESENT | PAGE_RW)) {
            PageAlloc::VirtualFreePages(vbase, want);
            PageAlloc::PhysicalFreePages(phys, want);
            Serial::Write("[DMA] MapPages failed for pool\n");
            return;
        }

        // Init globals
        g_poolPhys = phys;
        g_poolVirt = (UPTR)vbase;
        g_poolPages = want;

        // Allocate bitmap
        SIZE_T bytes = (g_poolPages + 7) / 8;
        g_bitmap = (U8*)kmalloc(bytes);
        if (!g_bitmap) {
            Serial::Write("[DMA] Failed to allocate bitmap, tearing down\n");
            PageAlloc::VirtualFreePages(vbase, want);
            PageAlloc::PhysicalFreePages(phys, want);
            g_poolPhys = g_poolVirt = g_poolPages = 0;
            return;
        }
        String::Memset(g_bitmap, 0, bytes);

        // Zero the pool for deterministic content
        String::Memset((void*)g_poolVirt, 0, (unsigned long long)g_poolPages * PAGE_SIZE);
        mfence();

        Serial::Printf("[DMA] Pool ready: phys=%p virt=%p pages=%u (bitmap=%u bytes)\n",
            (void*)(uintptr_t)g_poolPhys, (void*)(uintptr_t)g_poolVirt, (unsigned)g_poolPages, (unsigned)bytes);
    }

    // First-fit contiguous allocation of 'count' pages
    DMABuffer *AllocateDMAPages(SIZE_T count) {
        if (count == 0 || count > g_poolPages) return nullptr;

        // If pool hasn't been initialized (possible if called early or init failed),
        // attempt to initialize lazily and continue.
            if (g_poolPages == 0 || !g_bitmap) {
            Serial::Write("[DMA] AllocateDMAPages: pool not initialized, attempting InitializeDMA()\n");
            PageAlloc::DMAAlloc::InitializeDMA();
            if (g_poolPages == 0 || !g_bitmap) {
                Serial::Printf("[DMA] AllocateDMAPages: pool still not ready (pages=%u) - attempting fallback\n", (unsigned)g_poolPages);
                // Fallback: try to allocate dedicated physical+virtual pages and map them.
                UPTR phys = PageAlloc::PhysicalAllocPages(count);
                if (!phys) {
                    Serial::Printf("[DMA] Fallback PhysicalAllocPages failed for %u pages\n", (unsigned)count);
                    return nullptr;
                }
                void* v = PageAlloc::VirtualAllocPages(count);
                if (!v) {
                    Serial::Printf("[DMA] Fallback VirtualAllocPages failed for %u pages\n", (unsigned)count);
                    PageAlloc::PhysicalFreePages(phys, count);
                    return nullptr;
                }
                // Map the fallback range into kernel PML4
                if (!PageAlloc::MapPages(KernelPML4, phys, (UPTR)v, count, PAGE_PRESENT | PAGE_RW)) {
                    Serial::Printf("[DMA] Fallback MapPages failed for phys=%p virt=%p pages=%u\n", (void*)(uintptr_t)phys, v, (unsigned)count);
                    PageAlloc::VirtualFreePages(v, count);
                    PageAlloc::PhysicalFreePages(phys, count);
                    return nullptr;
                }

                DMABuffer* fb = (DMABuffer*)kmalloc(sizeof(DMABuffer));
                if (!fb) {
                    Serial::Printf("[DMA] Fallback kmalloc DMABuffer metadata failed\n");
                    PageAlloc::VirtualFreePages(v, count);
                    PageAlloc::PhysicalFreePages(phys, count);
                    return nullptr;
                }
                fb->FirstIndex = (SIZE_T)-1; // sentinel => fallback allocation
                fb->Pages = count;
                fb->Size = count * PAGE_SIZE;
                fb->PhysAddr = phys;
                fb->VirtAddr = (UPTR)v;
                // Zero and fence
                String::Memset((void*)fb->VirtAddr, 0, (unsigned long long)fb->Size);
                mfence();
                return fb;
            }
        }

        // We must disable interrupts while scanning and marking the bitmap so an
        // IRQ handler cannot concurrently allocate/free from the same bitmap and
        // cause races/corruption. Keep the critical section as short as possible
        // (we release before doing the possibly-large memset).
        LOCKRFLAGS lock = Arch::SaveAndDisballeInterrupts();

        SIZE_T run = 0;
        SIZE_T start = 0;
        for (SIZE_T i = 0; i < g_poolPages; ++i) {
            if (!bmp_test(i)) {
                if (run == 0) start = i;
                ++run;
                if (run == count) {
                    // mark allocated
                    for (SIZE_T j = 0; j < count; ++j) bmp_set(start + j);

                    // Allocate metadata while still in critical section to avoid
                    // races where an IRQ path might observe inconsistent bitmap.
                    DMABuffer* b = (DMABuffer*)kmalloc(sizeof(DMABuffer));
                    if (!b) {
                        // rollback
                        for (SIZE_T j = 0; j < count; ++j) bmp_clear(start + j);
                        Arch::RestoreInterrupts(lock);
                        return nullptr;
                    }
                    b->FirstIndex = start;
                    b->Pages = count;
                    b->Size = count * PAGE_SIZE;
                    b->PhysAddr = g_poolPhys + start * PAGE_SIZE;
                    b->VirtAddr = g_poolVirt + start * PAGE_SIZE;

                    // Release interrupts before performing the potentially large
                    // zeroing of the buffer to avoid long IRQ-disabled windows.
                    Arch::RestoreInterrupts(lock);

                    // Zero the allocated slice and fence
                    String::Memset((void*)b->VirtAddr, 0, (unsigned long long)b->Size);
                    mfence();
                    return b;
                }
            } else {
                run = 0;
            }
        }

        Arch::RestoreInterrupts(lock);
        return nullptr; // no space
    }

    void FreeDMAPages(UPTR addr, SIZE_T count) {
        // Basic sanity checks
        if (g_poolPages == 0 || !g_bitmap) return;
        if (count == 0) return;
        if (addr < g_poolVirt) return;

        UPTR end = g_poolVirt + g_poolPages * PAGE_SIZE;
        if (addr >= end) return;
        if ((addr - g_poolVirt) % PAGE_SIZE != 0) return; // require page aligned

        SIZE_T idx = (SIZE_T)((addr - g_poolVirt) / PAGE_SIZE);
        if (idx + count > g_poolPages) return;

        // Disable interrupts while we touch the bitmap to avoid races with IRQ code
        LOCKRFLAGS lock = Arch::SaveAndDisballeInterrupts();

        // Zero memory handed back to the allocator (helps detect use-after-free)
        String::Memset((void*)addr, 0, (unsigned long long)count * PAGE_SIZE);
        mfence();

        // Clear bits and check for double-free
        for (SIZE_T i = 0; i < count; ++i) {
            if (!bmp_test(idx + i)) {
                // double free or corruption detected
                Serial::Printf("[DMA] Warning: freeing page %u which is not marked allocated\n", (unsigned)(idx + i));
            }
            bmp_clear(idx + i);
        }

        mfence();
        Arch::RestoreInterrupts(lock);

        return;
    } 

    // Free by DMABuffer pointer (frees metadata too). This is the preferred API
    // when AllocateDMAPages() returned a DMABuffer*.
    void FreeDMABuffer(DMABuffer* b) {
        if (!b) return;
        UPTR addr = b->VirtAddr;
        SIZE_T count = b->Pages;
        // If this buffer came from fallback allocation (not the pool), free
        // the mapped pages and metadata.
        if (b->FirstIndex == (SIZE_T)-1) {
            if (count) {
                // Unmap/free the virtual and physical pages allocated in fallback
                PageAlloc::VirtualFreePages((void*)b->VirtAddr, count);
                PageAlloc::PhysicalFreePages(b->PhysAddr, count);
            }
            kfree(b);
            return;
        }

        if (g_poolPages == 0 || !g_bitmap) {
            // Nothing to do for metadata (pool absent)
            kfree(b);
            return;
        }

        if (count == 0) { kfree(b); return; }

        if (addr < g_poolVirt) { kfree(b); return; }

        UPTR end = g_poolVirt + g_poolPages * PAGE_SIZE;
        if (addr >= end) { kfree(b); return; }
        if ((addr - g_poolVirt) % PAGE_SIZE != 0) { kfree(b); return; }

        SIZE_T idx = (SIZE_T)((addr - g_poolVirt) / PAGE_SIZE);
        if (idx + count > g_poolPages) { kfree(b); return; }

        LOCKRFLAGS lock = Arch::SaveAndDisballeInterrupts();

        // Zero memory and fence before clearing ownership bits
        String::Memset((void*)addr, 0, (unsigned long long)count * PAGE_SIZE);
        mfence();

        for (SIZE_T i = 0; i < count; ++i) {
            if (!bmp_test(idx + i)) {
                Serial::Printf("[DMA] Warning: freeing page %u which is not marked allocated\n", (unsigned)(idx + i));
            }
            bmp_clear(idx + i);
        }
        mfence();

        Arch::RestoreInterrupts(lock);

        // Free the metadata allocated at allocation time
        kfree(b);
    }

} // namespace DMAAlloc
} // namespace PageAlloc