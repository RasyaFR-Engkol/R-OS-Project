#include "kmalloc.hpp"
#include "../mm.hpp"
#include "debug.hpp"
#include "rossys.hpp"
#include "rosval.h"
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

    static constexpr U64 KMALLOC_CANARY = 0xDEADCAFEDEADCAFEULL;

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
    static ExpectedCrash::ExpectCrash_T *EXP;
    

    void Init(SIZE_T pages) {
        if (pages == 0) pages = 16; // default 16 pages (~64KiB)

        EXP = ExpectedCrash::Create(11, "Kmalloc::Alloc");

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
    // Dump free-list for debugging
    UNUSED__ auto dump_free_list = [&](const char* ctx, SIZE_T maxNodes = 16) {
        Serial::Printf("[kmalloc] free-list dump (%s): head=%p\n", ctx, (void*)free_list);
        FreeBlock* n = free_list;
        for (SIZE_T k = 0; n && k < maxNodes; ++k, n = n->next) {
            Serial::Printf("  [%u] node=%p size=%llu magic=0x%x next=%p\n",
                (U32)k, (void*)n, (unsigned long long)n->size, n->magic, (void*)n->next);
        }
    };

    block->magic = MAGIC_FREE;
    //Serial::Printf("[kmalloc] insert_and_coalesce: block=%p size=%llu\n", (void*)block, (unsigned long long)block->size);

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
    // If the free_list head or any pointer looks suspicious, dump list
    if ((UPTR)free_list < (UPTR)heap_low || (UPTR)free_list >= (UPTR)heap_high) {
        //Serial::Printf("[kmalloc] insert_and_coalesce: free_list head %p outside heap bounds %p..%p\n", free_list, heap_low, heap_high);
        //dump_free_list("after-insert");
    }
    }

    void* Alloc(SIZE_T size) {
        if (size == 0) return nullptr;
        ExpectedCrash::Report(EXP);
        LOCKRFLAGS Kmallock = 0;
        Kmallock = Arch::SaveAndDisballeInterrupts();
        constexpr SIZE_T TAIL_CANARY_BYTES = sizeof(U64);
        if (size > ((SIZE_T)-1 - TAIL_CANARY_BYTES)) {
            Arch::RestoreInterrupts(Kmallock);
            return nullptr;
        }

        SIZE_T need = align_up(size + TAIL_CANARY_BYTES);
        SIZE_T total = need + HEADER_SIZE;
        FreeBlock* prev = nullptr;
        FreeBlock* cur = free_list;
        ExpectedCrash::Report(EXP);

        ExpectedCrash::Report(EXP);
        while (cur) {
            // Sanity checks to avoid page-fault when following a corrupted free list.
            if (!heap_low || !heap_high) {
                Serial::Printf("[kmalloc] heap bounds not set, aborting allocation\n");
                Arch::RestoreInterrupts(Kmallock);
                return nullptr;
            }
            UPTR cur_uaddr = (UPTR)cur;
            if (cur_uaddr < (UPTR)heap_low || cur_uaddr >= (UPTR)heap_high) {
                Serial::Printf("[kmalloc] free_list corrupted: node %p outside heap bounds %p..%p\n",
                    (void*)cur_uaddr, heap_low, heap_high);
                // avoid dereferencing cur further to prevent PF
                Arch::RestoreInterrupts(Kmallock);
                return nullptr;
            }
            // alignment check: nodes must be aligned to ALIGN
            if ((cur_uaddr & (ALIGN - 1)) != 0) {
                Serial::Printf("[kmalloc] free_list corrupted: node %p is unaligned (align %u)\n",
                    (void*)cur_uaddr, (U32)ALIGN);
                Arch::RestoreInterrupts(Kmallock);
                return nullptr;
            }
            // safe to read magic/size now
            if (cur->magic != MAGIC_FREE) {
                Serial::Printf("[kmalloc] free_list corruption: node %p has bad magic 0x%x\n",
                    (void*)cur_uaddr, cur->magic);
                Arch::RestoreInterrupts(Kmallock);
                return nullptr;
            }

            if (cur->size >= total) {
                ExpectedCrash::Report(EXP);
                // Found a block. If big enough to split, create a new free block
                if (cur->size >= total + HEADER_SIZE + ALIGN) {
                    // split
                    ExpectedCrash::Report(EXP);
                    UPTR cur_addr = (UPTR)cur;
                    UPTR next_addr = cur_addr + total;
                    FreeBlock* nb = (FreeBlock*)next_addr;
                    nb->size = cur->size - total;
                    nb->magic = MAGIC_FREE;
                    nb->next = cur->next;
                    ExpectedCrash::Report(EXP);

                    cur->size = total;
                    cur->magic = MAGIC_ALLOC;
                    cur->next = nullptr;
                    // write canary at end of user area for overflow detection
                    {
                        SIZE_T user_size = cur->size - HEADER_SIZE;
                        if (user_size >= sizeof(U64)) {
                            U8* user_ptr = (U8*)cur + HEADER_SIZE;
                            U8* canp8 = user_ptr + user_size - sizeof(U64);
                            for (int _k = 0; _k < (int)sizeof(U64); ++_k) {
                                canp8[_k] = (U8)((KMALLOC_CANARY >> (_k * 8)) & 0xFFULL);
                            }
                        }
                    }
                    ExpectedCrash::Report(EXP);

                    if (prev) prev->next = nb; else free_list = nb;
                    ExpectedCrash::Report(EXP);


                } else {
                    // use whole block
                    if (prev) prev->next = cur->next; else free_list = cur->next;
                    cur->magic = MAGIC_ALLOC;
                    cur->next = nullptr;
                    // write canary for whole-block allocation
                    {
                        SIZE_T user_size = cur->size - HEADER_SIZE;
                        if (user_size >= sizeof(U64)) {
                            U8* user_ptr = (U8*)cur + HEADER_SIZE;
                            U8* canp8 = user_ptr + user_size - sizeof(U64);
                            for (int _k = 0; _k < (int)sizeof(U64); ++_k) {
                                canp8[_k] = (U8)((KMALLOC_CANARY >> (_k * 8)) & 0xFFULL);
                            }
                        }
                    }
                    ExpectedCrash::Report(EXP);
                }

                // Return pointer after header
                void* user = (void*)((UPTR)cur + HEADER_SIZE);
                Arch::RestoreInterrupts(Kmallock);
                // sebelum bali, laporkan fungsi selesai
                ExpectedCrash::Report(EXP);
                ExpectedCrash::FunctionDone(EXP);
                return user;
            }
            prev = cur;
            cur = cur->next;
        }

        ExpectedCrash::FunctionDone(EXP);
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
                // sanity checks (same as above) to avoid deref of bad node
                UPTR cur_addr2 = (UPTR)cur;
                if (cur_addr2 < (UPTR)heap_low || cur_addr2 >= (UPTR)heap_high) {
                    Serial::Printf("[kmalloc] free_list corrupted (after grow): node %p outside heap bounds %p..%p\n",
                        (void*)cur_addr2, heap_low, heap_high);
                    break;
                }
                if ((cur_addr2 & (ALIGN - 1)) != 0) {
                    Serial::Printf("[kmalloc] free_list corrupted (after grow): node %p is unaligned (align %u)\n",
                        (void*)cur_addr2, (U32)ALIGN);
                    break;
                }
                if (cur->magic != MAGIC_FREE) {
                    Serial::Printf("[kmalloc] free_list corruption (after grow): node %p has bad magic 0x%x\n",
                        (void*)cur_addr2, cur->magic);
                    break;
                }

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
                        // write canary
                        {
                            SIZE_T user_size = cur->size - HEADER_SIZE;
                            if (user_size >= sizeof(U64)) {
                                U8* user_ptr = (U8*)cur + HEADER_SIZE;
                                U8* canp8 = user_ptr + user_size - sizeof(U64);
                                for (int _k = 0; _k < (int)sizeof(U64); ++_k) {
                                    canp8[_k] = (U8)((KMALLOC_CANARY >> (_k * 8)) & 0xFFULL);
                                }
                            }
                        }
                        if (prev) prev->next = nb; else free_list = nb;
                    } else {
                        if (prev) prev->next = cur->next; else free_list = cur->next;
                        cur->magic = MAGIC_ALLOC;
                        cur->next = nullptr;
                        // write canary
                        {
                            SIZE_T user_size = cur->size - HEADER_SIZE;
                            if (user_size >= sizeof(U64)) {
                                U8* user_ptr = (U8*)cur + HEADER_SIZE;
                                U8* canp8 = user_ptr + user_size - sizeof(U64);
                                for (int _k = 0; _k < (int)sizeof(U64); ++_k) {
                                    canp8[_k] = (U8)((KMALLOC_CANARY >> (_k * 8)) & 0xFFULL);
                                }
                            }
                        }
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
            Printk::Panic("[kmalloc] invalid or double free at %p (magic=0x%x)\n", ptr, block->magic);
            Arch::RestoreInterrupts(Kmallock);
            return;
        }
        //Serial::Printf("[kmalloc] Free: block=%p size=%llu\n", (void*)block, (unsigned long long)block->size);
        // check canary for buffer overflow detection
        {
            SIZE_T user_size = block->size - HEADER_SIZE;
            if (user_size >= sizeof(U64)) {
                U8* user_ptr = (U8*)block + HEADER_SIZE;
                U8* canp8 = user_ptr + user_size - sizeof(U64);
                U64 got = 0;
                for (int _k = 0; _k < (int)sizeof(U64); ++_k) {
                    got |= ((U64)canp8[_k]) << (_k * 8);
                }
                if (got != KMALLOC_CANARY) {
                    Printk::Write(Printk::Level::LOG_EMERG, "[kmalloc] buffer overflow detected at %p (canary mismatch: 0x%llx)\n", ptr, (unsigned long long)got);
                    // dump nearby memory and free-list head for investigation
                    U8* dump_start = (U8*)block;
                    Serial::Printf("[kmalloc] memory around block %p:\n", (void*)block);
                    for (int i = -16; i < 48; i += 16) {
                        U8* addr = dump_start + i;
                        Serial::Printf("  %p:", (void*)addr);
                        for (int j = 0; j < 16; ++j) {
                            Serial::Printf(" %02x", (unsigned)addr[j]);
                        }
                        Serial::Write("\n");
                    }
                    // dump a few free-list nodes
                    Serial::Printf("[kmalloc] free-list head=%p\n", (void*)free_list);
                    FreeBlock* n = free_list;
                    for (int k = 0; n && k < 8; ++k, n = n->next) {
                        Serial::Printf("  [%d] node=%p size=%llu magic=0x%x next=%p\n",
                            k, (void*)n, (unsigned long long)n->size, n->magic, (void*)n->next);
                    }
                }
            }
        }
        block->magic = MAGIC_FREE;

        insert_and_coalesce(block);

        Arch::RestoreInterrupts(Kmallock);
    }

} // namespace Kmalloc
