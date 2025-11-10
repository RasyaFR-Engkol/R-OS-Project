#include "kmalloc.hpp"
#include "../mm.hpp"
#include "rossys.hpp"
#include <string.hpp>
#include <serial.hpp>

#define PRINTK_MODULE_NAME "kmalloc"
#include <logging.hpp>

namespace Kmalloc {

    // Block header stored at the start of each free block.
    struct FreeBlock {
        SIZE_T size; // size of this block including header
        U32 magic;   // marker to detect double free/corruption
        FreeBlock* next;
    };

    static FreeBlock* free_list = nullptr;
    static void* heap_start = nullptr;
    static SIZE_T heap_size = 0; // in bytes
    static void* heap_low = nullptr;
    static void* heap_high = nullptr; // exclusive

    static constexpr SIZE_T ALIGN = 8;
    static inline SIZE_T align_up(SIZE_T v) { return (v + (ALIGN - 1)) & ~(ALIGN - 1); }
    static constexpr U32 MAGIC_FREE  = 0xfeedf00dU;
    static constexpr U32 MAGIC_ALLOC = 0xcafebabeU;
    static constexpr SIZE_T HEADER_SIZE = ((sizeof(FreeBlock) + (ALIGN - 1)) & ~(ALIGN - 1));

    void Init(SIZE_T pages) {
        if (pages == 0) pages = 16; // default 16 pages (~64KiB)

        // Allocate virtual pages from virtual allocator
        void* vaddr = PageAlloc::VirtualAllocPages(pages);
        if (!vaddr) {
            Serial::Write("[kmalloc] VirtualAllocPages failed\n");
            return;
        }

        // For each virtual page, allocate a physical page and map it
        UPTR base = (UPTR)vaddr;
        for (SIZE_T i = 0; i < pages; ++i) {
            UPTR phys = PageAlloc::PhysicalAllocPages(1);
            if (phys == 0) {
                Serial::Printf("[kmalloc] PhysicalAllocPages failed at page %u\n", (U32)i);
                // Do not try to unwind; just stop mapping further pages
                break;
            }
            // Map 4K page: use PML4 HHDM virtual pointer
            U64* pml4 = HHDM_PhysToVirt(KernelPML4Phys);
            if (!PageAlloc::MapPages(pml4, phys, base + i * PAGE_SIZE, 1, PAGE_PRESENT | PAGE_RW)) {
                Serial::Printf("[kmalloc] MapPages failed at page %u\n", (U32)i);
            }
        }

        heap_start = vaddr;
        heap_size = pages * PAGE_SIZE;

        // Initialize freelist with one big free block covering the whole heap
    free_list = (FreeBlock*)heap_start;
    free_list->size = heap_size;
    free_list->magic = MAGIC_FREE;
    free_list->next = nullptr;

        heap_low = heap_start;
        heap_high = (void*)((UPTR)heap_start + heap_size);
        Serial::Printf("[kmalloc] heap @ %p size=%llu bytes\n", heap_start, (unsigned long long)heap_size);
    }

    static void insert_and_coalesce(FreeBlock* block) {
    block->magic = MAGIC_FREE;

    // Insert block into free list sorted by address
        if (!free_list) { block->next = nullptr; free_list = block; return; }
        FreeBlock* prev = nullptr;
        FreeBlock* cur = free_list;
        UPTR baddr = (UPTR)block;
        while (cur && (UPTR)cur < baddr) { prev = cur; cur = cur->next; }

        // Insert between prev and cur
        block->next = cur;
        if (prev) prev->next = block; else free_list = block;

        // Try coalescing with next
        if (block->next) {
            UPTR end_block = (UPTR)block + block->size;
            if (end_block == (UPTR)block->next) {
                block->size += block->next->size;
                block->next = block->next->next;
            }
        }
        // Try coalescing with prev
        if (prev) {
            UPTR end_prev = (UPTR)prev + prev->size;
            if (end_prev == (UPTR)block) {
                prev->size += block->size;
                prev->next = block->next;
            }
        }
    }

    void* Alloc(SIZE_T size) {
        if (size == 0) return nullptr;
        LOCKRFLAGS Kmallock = 0;
        Kmallock = Arch::SaveAndDisballeInterrupts();
        SIZE_T need = align_up(size);
        SIZE_T total = need + HEADER_SIZE;

        FreeBlock* prev = nullptr;
        FreeBlock* cur = free_list;
        while (cur) {
            if (cur->size >= total) {
                // Found a block. If big enough to split, create a new free block
                if (cur->size >= total + HEADER_SIZE + ALIGN) {
                    // split
                    UPTR cur_addr = (UPTR)cur;
                    UPTR next_addr = cur_addr + total;
                    FreeBlock* nb = (FreeBlock*)next_addr;
                    nb->size = cur->size - total;
                    nb->magic = MAGIC_FREE;
                    nb->next = cur->next;

                    cur->size = total;
                    cur->magic = MAGIC_ALLOC;
                    cur->next = nullptr;

                    if (prev) prev->next = nb; else free_list = nb;
                } else {
                    // use whole block
                    if (prev) prev->next = cur->next; else free_list = cur->next;
                    cur->magic = MAGIC_ALLOC;
                    cur->next = nullptr;
                }

                // Return pointer after header
                void* user = (void*)((UPTR)cur + HEADER_SIZE);
                Arch::RestoreInterrupts(Kmallock);
                return user;
            }
            prev = cur;
            cur = cur->next;
        }

        // No block found: try growing the heap and retry once
        // Determine pages to grow (at least enough to satisfy 'total', up to a chunk)
    auto pagesNeeded = (total + PAGE_SIZE - 1) / PAGE_SIZE;
        SIZE_T growPages = pagesNeeded;
        const SIZE_T GROW_CHUNK = 16; // grow by up to 16 pages at once for efficiency
        if (growPages < GROW_CHUNK) growPages = GROW_CHUNK;

        // Attempt to grow
        Serial::Printf("[kmalloc] no-fit, attempting to grow heap by %u pages\n", (U32)growPages);
        // Grow helper
        auto grow_ok = [&]() -> bool {
            void* vaddr = PageAlloc::VirtualAllocPages(growPages);
            if (!vaddr) {
                Arch::RestoreInterrupts(Kmallock);
                return false;
            }
            UPTR base = (UPTR)vaddr;
            SIZE_T mapped = 0;
            U64* pml4 = HHDM_PhysToVirt(KernelPML4Phys);
            for (SIZE_T i = 0; i < growPages; ++i) {
                UPTR phys = PageAlloc::PhysicalAllocPages(1);
                if (phys == 0) break;
                if (!PageAlloc::MapPages(pml4, phys, base + i * PAGE_SIZE, 1, PAGE_PRESENT | PAGE_RW)) break;
                ++mapped;
            }
            if (mapped == 0) {
                Arch::RestoreInterrupts(Kmallock);
                return false;
            }

            // Create a new free block for the mapped region and insert it
            FreeBlock* nb = (FreeBlock*)base;
            nb->size = mapped * PAGE_SIZE;
            nb->magic = MAGIC_FREE;
            nb->next = nullptr;
            insert_and_coalesce(nb);

            // Update heap_size and bounds (heap_start remains the original base)
            heap_size += mapped * PAGE_SIZE;
            // adjust low/high bounds in case new region is outside original range
            if (!heap_low || base < (UPTR)heap_low) heap_low = (void*)base;
            void* new_high = (void*)(base + mapped * PAGE_SIZE);
            if (!heap_high || new_high > heap_high) heap_high = new_high;
            Serial::Printf("[kmalloc] grew heap: added %u pages (%llu bytes)\n", (U32)mapped, (unsigned long long)mapped * PAGE_SIZE);
            Arch::RestoreInterrupts(Kmallock);
            return true;
        };

        if (grow_ok()) {
            // Try allocation again once
            prev = nullptr;
            cur = free_list;
            while (cur) {
                if (cur->size >= total) {
                    if (cur->size >= total + HEADER_SIZE + ALIGN) {
                        UPTR cur_addr = (UPTR)cur;
                        UPTR next_addr = cur_addr + total;
                        FreeBlock* nb = (FreeBlock*)next_addr;
                        nb->size = cur->size - total;
                        nb->magic = MAGIC_FREE;
                        nb->next = cur->next;
                        cur->size = total;
                        cur->magic = MAGIC_ALLOC;
                        cur->next = nullptr;
                        if (prev) prev->next = nb; else free_list = nb;
                    } else {
                        if (prev) prev->next = cur->next; else free_list = cur->next;
                        cur->magic = MAGIC_ALLOC;
                        cur->next = nullptr;
                    }
                    void* user = (void*)((UPTR)cur + HEADER_SIZE);
                    Arch::RestoreInterrupts(Kmallock);
                    return user;
                }
                prev = cur;
                cur = cur->next;
            }
        }

        Arch::RestoreInterrupts(Kmallock);
        return nullptr;
    }

    

    void Free(void* ptr) {
        if (!ptr) return;
        LOCKRFLAGS Kmallock = 0;
        Kmallock = Arch::SaveAndDisballeInterrupts();
        // Header is located before user pointer
        UPTR u = (UPTR)ptr;
        UPTR hdr = u - HEADER_SIZE;
        FreeBlock* block = (FreeBlock*)hdr;
        // Basic sanity: block must be within any mapped heap region
        if (!heap_low || !heap_high) {
            Arch::RestoreInterrupts(Kmallock);
            return;
        }
        if ((UPTR)block < (UPTR)heap_low || (UPTR)block >= (UPTR)heap_high) {
            Arch::RestoreInterrupts(Kmallock);
            return;
        }
        if (block->magic != MAGIC_ALLOC) {
            Printk::Write(Printk::Level::LOG_EMERG, "[kmalloc] invalid or double free at %p (magic=0x%x)\n", ptr, block->magic);
            Arch::RestoreInterrupts(Kmallock);
            return;
        }
        block->magic = MAGIC_FREE;

        insert_and_coalesce(block);


        Arch::RestoreInterrupts(Kmallock);
    }

} // namespace Kmalloc
