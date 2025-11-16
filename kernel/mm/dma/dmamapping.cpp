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
    constexpr SIZE_T DMA_GUARD_PAGES_DEFAULT = 1; // add a guard page per allocation by default
    constexpr U8 DMA_GUARD_PATTERN = 0xA5;

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

    static inline SIZE_T guard_bytes(const PageAlloc::DMAAlloc::DMABuffer* buf) {
        return (buf && buf->GuardPages) ? buf->GuardPages * PAGE_SIZE : 0;
    }

    static inline void fill_guard_region(PageAlloc::DMAAlloc::DMABuffer* buf) {
        SIZE_T bytes = guard_bytes(buf);
        if (!bytes) return;
        void* guardPtr = (void*)(uintptr_t)(buf->VirtAddr + buf->Pages * PAGE_SIZE);
        String::Memset(guardPtr, DMA_GUARD_PATTERN, (unsigned long long)bytes);
    }

    static inline bool guard_region_intact(const PageAlloc::DMAAlloc::DMABuffer* buf) {
        SIZE_T bytes = guard_bytes(buf);
        if (!bytes) return true;
        const U8* guardPtr = (const U8*)(uintptr_t)(buf->VirtAddr + buf->Pages * PAGE_SIZE);
        for (SIZE_T i = 0; i < bytes; ++i) {
            if (guardPtr[i] != DMA_GUARD_PATTERN) return false;
        }
        return true;
    }
}

namespace PageAlloc {
namespace DMAAlloc {

    struct DMAStats {
        SIZE_T pool_pages;
        SIZE_T pool_free_pages;
        SIZE_T alloc_calls;
        SIZE_T fallback_allocs;
        SIZE_T failed_allocs;
        SIZE_T freed_buffers;
    };

    static DMAStats g_stats{};

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

        g_stats.pool_pages = g_poolPages;
        Serial::Printf("[DMA] Pool ready: phys=%p virt=%p pages=%u (bitmap=%u bytes)\n",
            (void*)(uintptr_t)g_poolPhys, (void*)(uintptr_t)g_poolVirt, (unsigned)g_poolPages, (unsigned)bytes);
    }

    // First-fit contiguous allocation of 'count' pages
    static inline SIZE_T FreePagesUnsafe() {
        // Count free bits; O(n). For debugging only (not used on hot path except stats).
        SIZE_T freeCount = 0;
        for (SIZE_T i = 0; i < g_poolPages; ++i) {
            if (!bmp_test(i)) ++freeCount;
        }
        return freeCount;
    }

    DMABuffer *AllocateDMAPages(SIZE_T count) {
        if (count == 0) return nullptr;
        ++g_stats.alloc_calls;

        // Lazily initialize the pool if needed.
        if (g_poolPages == 0 || !g_bitmap) {
            Serial::Write("[DMA] AllocateDMAPages: pool not initialized, attempting InitializeDMA()\n");
            PageAlloc::DMAAlloc::InitializeDMA();
        }

        const SIZE_T guardOptions[2] = { DMA_GUARD_PAGES_DEFAULT, 0 };

        // Primary attempt: try to satisfy from the pooled bitmap allocator.
        if (g_poolPages != 0 && g_bitmap) {
            for (SIZE_T attempt = 0; attempt < 2; ++attempt) {
                SIZE_T guardPages = guardOptions[attempt];
                if (guardPages && count > (((SIZE_T)-1) - guardPages)) guardPages = 0;
                SIZE_T totalNeeded = count + guardPages;
                if (totalNeeded == 0) continue;
                if (totalNeeded > g_poolPages) continue;

                LOCKRFLAGS lock = Arch::SaveAndDisballeInterrupts();
                SIZE_T run = 0;
                SIZE_T start = 0;
                for (SIZE_T i = 0; i < g_poolPages; ++i) {
                    if (!bmp_test(i)) {
                        if (run == 0) start = i;
                        ++run;
                        if (run == totalNeeded) {
                            for (SIZE_T j = 0; j < totalNeeded; ++j) bmp_set(start + j);

                            DMABuffer* b = (DMABuffer*)kmalloc(sizeof(DMABuffer));
                            if (!b) {
                                for (SIZE_T j = 0; j < totalNeeded; ++j) bmp_clear(start + j);
                                Arch::RestoreInterrupts(lock);
                                return nullptr;
                            }

                            b->FirstIndex = start;
                            b->Pages = count;
                            b->GuardPages = guardPages;
                            b->TotalPages = totalNeeded;
                            b->Size = count * PAGE_SIZE;
                            b->RequestedBytes = b->Size;
                            b->PhysAddr = g_poolPhys + start * PAGE_SIZE;
                            b->VirtAddr = g_poolVirt + start * PAGE_SIZE;

                            Arch::RestoreInterrupts(lock);

                            String::Memset((void*)b->VirtAddr, 0, (unsigned long long)b->Size);
                            fill_guard_region(b);
                            mfence();
                            return b;
                        }
                    } else {
                        run = 0;
                    }
                }
                Arch::RestoreInterrupts(lock);
            }
        }

        // Fallback: allocate dedicated physical/virtual pages outside the pool.
        for (SIZE_T attempt = 0; attempt < 2; ++attempt) {
            SIZE_T guardPages = guardOptions[attempt];
            if (guardPages && count > (((SIZE_T)-1) - guardPages)) guardPages = 0;
            SIZE_T totalPages = count + guardPages;
            if (totalPages == 0) continue;

            UPTR phys = PageAlloc::PhysicalAllocPages(totalPages);
            if (!phys) continue;

            void* v = PageAlloc::VirtualAllocPages(totalPages);
            if (!v) {
                PageAlloc::PhysicalFreePages(phys, totalPages);
                continue;
            }

            if (!PageAlloc::MapPages(KernelPML4, phys, (UPTR)v, totalPages, PAGE_PRESENT | PAGE_RW)) {
                PageAlloc::VirtualFreePages(v, totalPages);
                PageAlloc::PhysicalFreePages(phys, totalPages);
                continue;
            }

            DMABuffer* fb = (DMABuffer*)kmalloc(sizeof(DMABuffer));
            if (!fb) {
                PageAlloc::VirtualFreePages(v, totalPages);
                PageAlloc::PhysicalFreePages(phys, totalPages);
                continue;
            }

            fb->FirstIndex = (SIZE_T)-1;
            fb->Pages = count;
            fb->GuardPages = guardPages;
            fb->TotalPages = totalPages;
            fb->Size = count * PAGE_SIZE;
            fb->RequestedBytes = fb->Size;
            fb->PhysAddr = phys;
            fb->VirtAddr = (UPTR)v;

            String::Memset((void*)fb->VirtAddr, 0, (unsigned long long)fb->Size);
            fill_guard_region(fb);
            mfence();
            ++g_stats.fallback_allocs;
            return fb;
        }

        ++g_stats.failed_allocs;
        return nullptr;
    }

    DMABuffer *AllocateDMABytes(SIZE_T bytes) {
        if (bytes == 0) return nullptr;
        SIZE_T pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
        DMABuffer* buf = AllocateDMAPages(pages);
        if (buf) buf->RequestedBytes = bytes;
        return buf;
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
        const SIZE_T usablePages = b->Pages;
        const SIZE_T guardPages = b->GuardPages;
        const SIZE_T totalPages = usablePages + guardPages;
        const SIZE_T usableBytes = usablePages * PAGE_SIZE;
        const SIZE_T guardBytes = guardPages * PAGE_SIZE;

        //Serial::Printf("[DMA] Free meta=%p phys=%p virt=%p pages=%llu guard=%llu total=%llu req=%llu first=%lld\n",
          //  (void*)b,
          //  (void*)(uintptr_t)b->PhysAddr,
          //  (void*)(uintptr_t)addr,
          //  (unsigned long long)usablePages,
          //  (unsigned long long)guardPages,
           // (unsigned long long)totalPages,
          //  (unsigned long long)b->RequestedBytes,
          //  (long long)b->FirstIndex);

        if (!guard_region_intact(b)) {
            Printk::Write(Printk::Level::LOG_EMERG,
                "[DMA] Guard corruption detected virt=%p phys=%p requested=%llu size=%llu guardPages=%llu\n",
                (void*)(uintptr_t)addr,
                (void*)(uintptr_t)b->PhysAddr,
                (unsigned long long)b->RequestedBytes,
                (unsigned long long)b->Size,
                (unsigned long long)guardPages);
            Serial::Printf("[DMA] Guard pattern corrupted for buffer %p (phys=%p, requested=%llu, guard=%llu pages)\n",
                (void*)(uintptr_t)addr,
                (void*)(uintptr_t)b->PhysAddr,
                (unsigned long long)b->RequestedBytes,
                (unsigned long long)guardPages);
        }

        if (b->FirstIndex == (SIZE_T)-1) {
            if (totalPages) {
                if (usableBytes) {
                    String::Memset((void*)addr, 0, (unsigned long long)usableBytes);
                }
                if (guardBytes) {
                    void* guardPtr = (void*)(uintptr_t)(addr + usableBytes);
                    String::Memset(guardPtr, 0, (unsigned long long)guardBytes);
                }
                mfence();
                PageAlloc::VirtualFreePages((void*)addr, totalPages);
                PageAlloc::PhysicalFreePages(b->PhysAddr, totalPages);
            }
            kfree(b);
            ++g_stats.freed_buffers;
            return;
        }

        if (g_poolPages == 0 || !g_bitmap) {
            kfree(b);
            return;
        }

        if (totalPages == 0) { kfree(b); return; }
        if (addr < g_poolVirt) { kfree(b); return; }

        UPTR end = g_poolVirt + g_poolPages * PAGE_SIZE;
        if (addr >= end) { kfree(b); return; }
        if ((addr - g_poolVirt) % PAGE_SIZE != 0) { kfree(b); return; }

        SIZE_T idx = (SIZE_T)((addr - g_poolVirt) / PAGE_SIZE);
        if (idx + totalPages > g_poolPages) { kfree(b); return; }

        LOCKRFLAGS lock = Arch::SaveAndDisballeInterrupts();

        if (usableBytes) {
            String::Memset((void*)addr, 0, (unsigned long long)usableBytes);
        }
        if (guardBytes) {
            void* guardPtr = (void*)(uintptr_t)(addr + usableBytes);
            String::Memset(guardPtr, 0, (unsigned long long)guardBytes);
        }
        mfence();

        for (SIZE_T i = 0; i < totalPages; ++i) {
            if (!bmp_test(idx + i)) {
                Serial::Printf("[DMA] Warning: freeing page %u which is not marked allocated\n", (unsigned)(idx + i));
            }
            bmp_clear(idx + i);
        }
        mfence();

        Arch::RestoreInterrupts(lock);

        kfree(b);
        ++g_stats.freed_buffers;
    }

    // Debug: snapshot stats (counts free pages with O(n) scan)
    void GetStats(SIZE_T *poolPages, SIZE_T *freePages, SIZE_T *allocCalls,
                  SIZE_T *fallbacks, SIZE_T *failed, SIZE_T *freed) {
        if (poolPages) *poolPages = g_stats.pool_pages;
        if (freePages) *freePages = (g_poolPages ? FreePagesUnsafe() : 0);
        if (allocCalls) *allocCalls = g_stats.alloc_calls;
        if (fallbacks) *fallbacks = g_stats.fallback_allocs;
        if (failed) *failed = g_stats.failed_allocs;
        if (freed) *freed = g_stats.freed_buffers;
    }

} // namespace DMAAlloc
} // namespace PageAlloc