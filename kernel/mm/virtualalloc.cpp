#include "bootinfo.h"
#include "mm.hpp"
#include <rossys.hpp>
// Use project-provided basic types to avoid relying on host libc headers
#include <rosval.h>
#include <logging.hpp>
#include <export_sym.hpp>

// Simple bitmap-backed virtual page allocator.
// This implementation is intentionally small and configurable via the
// constants below. It provides a contiguous-page allocator from a fixed
// virtual pool. Adjust POOL_BASE and POOL_PAGES to match your kernel's
// virtual layout.

extern "C" char __kernel_virt_end; // from linker script

namespace {
    struct VMARegion {
        UPTR StartPage;
        SIZE_T Count;
        VMARegion *Next;
    };

    CONSTANTEXPR I32 MAX_VMA_NODES = 4096;
    static VMARegion node_pool[MAX_VMA_NODES];

    STATIC VMARegion *free_nodes_list = nullptr;
    STATIC VMARegion *free_vma_list = nullptr;

    UPTR g_pool_base = 0;

    VMARegion* AllocNode() {
        if (!free_nodes_list) return nullptr; // Pool habis (sangat jarang jika 4096)
        VMARegion* node = free_nodes_list;
        free_nodes_list = free_nodes_list->Next;
        node->Next = nullptr;
        return node;
    }

    void FreeNode(VMARegion* node) {
        if (!node) return;
        node->Next = free_nodes_list;
        free_nodes_list = node;
    }
}


namespace PageAlloc{
    void Virtual() {
        for(int i = 0; i < MAX_VMA_NODES - 1; i++){
            node_pool[i].Next = &node_pool[i + 1];
        }
        node_pool[MAX_VMA_NODES - 1].Next = nullptr;
        free_nodes_list = &node_pool[0];

        UPTR end = (UPTR)&__kernel_virt_end;
        const UPTR TWO_MB = 0x200000ULL;
        g_pool_base = (end + (TWO_MB - 1)) & ~(TWO_MB - 1);

        SIZE_T total_virtual_pages = 65536;

        const BootInfo *boot = BootInfoGet();
        if(boot && boot->has_memmap){
            U64 highest_physical_addr = 0;
            
            for (U32 i = 0; i < boot->memmap.count; i++) {
                const BootMemRegion &r = boot->memmap.regions[i];
                // Cek semua tipe memori buat cari batas paling atas
                U64 region_end = r.base + r.length;
                if (region_end > highest_physical_addr) {
                    highest_physical_addr = region_end;
                }
            }

            U64 virtual_bytes_needed = highest_physical_addr + 0x100000000ULL; 
            total_virtual_pages = virtual_bytes_needed / PAGE_SIZE;
            
            Printk::Write(Printk::Level::LOG_INFO, 
                "[VMM] Dynamic Virtual Pool Size: %llu GB\n", virtual_bytes_needed / (1024*1024*1024));
        }

        free_vma_list = AllocNode();
        free_vma_list->StartPage = g_pool_base / PAGE_SIZE;
        free_vma_list->Count = total_virtual_pages;
    }

    void* VirtualAllocPages(SIZE_T count) {
        if (count == 0) return nullptr;
        LOCKRFLAGS _irq = Arch::SaveAndDisableInterrupts();

        VMARegion *curr = free_vma_list;
        VMARegion *prev = nullptr;

        while(curr){
            if(curr->Count >= count){
                UPTR alloc_start_page = curr->StartPage;

                if(curr->Count == count){
                    if(prev) prev->Next = curr->Next;
                    else free_vma_list = curr->Next;
                    FreeNode(curr);
                } else {
                    curr->StartPage += count;
                    curr->Count -= count;
                }

                UPTR addr = alloc_start_page * PAGE_SIZE;
                Arch::RestoreInterrupts(_irq);
                return (VOID*)addr;
            }
            prev = curr;
            curr = curr->Next;
        }

        Arch::RestoreInterrupts(_irq);
        Printk::Write(Printk::Level::LOG_ERR, "[VMM] OUT OF VIRTUAL ADDRESS SPACE!\n");
        return nullptr;
    }

    void VirtualFreePages(void* addr, SIZE_T count) {
        if (addr == nullptr || count == 0) return;
        UPTR start_page = (UPTR)addr / PAGE_SIZE;
        
        LOCKRFLAGS _irq = Arch::SaveAndDisableInterrupts();

        VMARegion* curr = free_vma_list;
        VMARegion* prev = nullptr;

        // 1. Cari posisi sisip yang pas (sorting ASCENDING berdasarkan StartPage)
        while (curr && curr->StartPage < start_page) {
            prev = curr;
            curr = curr->Next;
        }

        // Sekarang kita tahu blok memori ini harus masuk di antara 'prev' dan 'curr'
        UNUSED__ bool merged_with_prev = false;

        // 2. Coba gabung (merge) dengan blok SEBELUMNYA (kiri)
        if (prev && (prev->StartPage + prev->Count == start_page)) {
            prev->Count += count; // Gabungin ukurannya ke blok prev
            merged_with_prev = true;
            
            // 3. Cek apakah prev sekarang malah nempel ke blok SETELAHNYA (kanan)
            if (curr && (prev->StartPage + prev->Count == curr->StartPage)) {
                prev->Count += curr->Count; // Caplok ukuran blok kanan
                prev->Next = curr->Next;    // Lompatin node curr
                FreeNode(curr);             // Buang node curr kembali ke pool
            }
        } 
        else {
            // Kalau GA BISA gabung dengan blok sebelumnya, kita harus bikin node baru
            VMARegion* new_free = AllocNode();
            if (!new_free) {
                // Sangat jarang terjadi jika MAX_VMA_NODES cukup
                Printk::Write(Printk::Level::LOG_ERR, "[VMM] Failed to allocate VMA node on Free!\n");
                Arch::RestoreInterrupts(_irq);
                return;
            }
            
            new_free->StartPage = start_page;
            new_free->Count = count;
            
            // Sisipkan node baru ke dalam linked list
            new_free->Next = curr;
            if (prev) {
                prev->Next = new_free;
            } else {
                // Kalau nggak ada prev, berarti dia jadi kepala list yang baru
                free_vma_list = new_free;
            }

            // 4. Cek apakah node baru ini nempel dengan blok SETELAHNYA (kanan)
            if (curr && (new_free->StartPage + new_free->Count == curr->StartPage)) {
                new_free->Count += curr->Count; // Caplok ukuran blok kanan
                new_free->Next = curr->Next;    // Lompatin node curr
                FreeNode(curr);                 // Buang node curr kembali ke pool
            }
        }

        Arch::RestoreInterrupts(_irq);
    }
}

ABI_C {
    // FOR MODULES
    VOID* MmVirtualAllocPages(SIZE_T count) {
        return PageAlloc::VirtualAllocPages(count);
    }
    EXPORT_SYMBOL(MmVirtualAllocPages);

    VOID MmVirtualFreePages(VOID* addr, SIZE_T count) {
        PageAlloc::VirtualFreePages(addr, count);
    }
    EXPORT_SYMBOL(MmVirtualFreePages);
}