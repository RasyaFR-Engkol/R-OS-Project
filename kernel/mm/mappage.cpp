#include <rosval.h>
#include "export_sym.hpp"
#include "mm.hpp"
#include "serial.hpp"
#include <string.hpp>
#include <rossys.hpp>

namespace Paging{
    BOOL MapPages(U64 *PML4Virt, UPTR PhysAddr, UPTR VirtAddr, SIZE_T Count, U64 Flags){
        LOCKRFLAGS _irq = Arch::SaveAndDisableInterrupts();
        for(SIZE_T I = 0; I < Count; ++I) {
            UPTR CurrentVirt = VirtAddr + (I * PAGE_SIZE);
            UPTR CurrentPhys = PhysAddr + (I * PAGE_SIZE);

            // 1. Level 4: PML4 (Page Map Level 4)
            SIZE_T PML4_IDX = (CurrentVirt >> 39) & 0x1FF;
            U64 *PML4E = &PML4Virt[PML4_IDX];

            U64* PDPTVirt;
            if(!(*PML4E & PAGE_PRESENT)) {
                UPTR NewPDPTPhys = PageAlloc::PhysicalAllocPages(1);
                if(NewPDPTPhys == 0) { Arch::RestoreInterrupts(_irq); return FALSE; }

                PDPTVirt = (U64*)HHDM_PhysToVirt(NewPDPTPhys); // Cast to U64*
                String::Memset(PDPTVirt, 0, PAGE_SIZE);

                *PML4E = NewPDPTPhys | PAGE_PRESENT | PAGE_RW | PAGE_USER;
            } else {
                // FIX: Pastikan Directory Entry ini RW!
                // Kalau sebelumnya Read-Only, kita "Jebol" jadi RW agar anak-anaknya bisa ditulis.
                if (!(*PML4E & PAGE_RW)) *PML4E |= PAGE_RW; 

                PDPTVirt = (U64*)HHDM_PhysToVirt(*PML4E & PAGE_ADDR_MASK);
            }

            // 2. Level 3: PDPT
            SIZE_T PDPTIDX = (CurrentVirt >> 30) & 0x1FF;
            U64* PDPTe = &PDPTVirt[PDPTIDX];

            U64* PDVirt;
            if(!(*PDPTe & PAGE_PRESENT)) {
                UPTR NewPDPhys = PageAlloc::PhysicalAllocPages(1);
                if(NewPDPhys == 0) { Arch::RestoreInterrupts(_irq); return FALSE; }

                PDVirt = (U64*)HHDM_PhysToVirt(NewPDPhys);
                String::Memset(PDVirt, 0, PAGE_SIZE);
                *PDPTe = NewPDPhys | PAGE_PRESENT | PAGE_RW | PAGE_USER;
            } else {
                 if (*PDPTe & PAGE_PS) {
                    // ... error handling ...
                    return FALSE; 
                }
                
                // FIX: Pastikan Entry ini RW!
                if (!(*PDPTe & PAGE_RW)) *PDPTe |= PAGE_RW;

                PDVirt = (U64*)HHDM_PhysToVirt(*PDPTe & PAGE_ADDR_MASK);
            }

            // 3. Level 2: PD
            SIZE_T PDIDX = (CurrentVirt >> 21) & 0x1FF;
            U64 *PDe = &PDVirt[PDIDX];

            U64 *PTVirt;
            if(!(*PDe & PAGE_PRESENT)) {
                UPTR NewPTPhys = PageAlloc::PhysicalAllocPages(1);
                if(NewPTPhys == 0) { Arch::RestoreInterrupts(_irq); return FALSE; }

                PTVirt = (U64*)HHDM_PhysToVirt(NewPTPhys);
                String::Memset(PTVirt, 0, PAGE_SIZE);
                *PDe = NewPTPhys | PAGE_PRESENT | PAGE_RW | PAGE_USER;
            } else {
                if(*PDe & PAGE_PS) {
                    // ... error handling ...
                    Arch::RestoreInterrupts(_irq);
                    return FALSE;
                }

                // FIX: Pastikan Entry ini RW!
                if (!(*PDe & PAGE_RW)) *PDe |= PAGE_RW;

                PTVirt = (U64*)HHDM_PhysToVirt(*PDe & PAGE_ADDR_MASK);
            }

            // 4. Level 1: PT (PAGE_TABLE)
            SIZE_T PTIDX = (CurrentVirt >> 12) & 0x01FF;
            U64 *PTE = &PTVirt[PTIDX];

            // Set flags final untuk halaman ini
            *PTE = CurrentPhys | Flags;

        }

        U64 CurrentCR3;
        asm volatile("mov %%cr3, %0" : "=r"(CurrentCR3));
        asm volatile("mov %0, %%cr3" :: "r"(CurrentCR3) : "memory");

        Arch::RestoreInterrupts(_irq);
        return TRUE;
    }

    BOOL UnmapPage(U64 *PML4Virt, UPTR VirtAddr) {
        LOCKRFLAGS _irq = Arch::SaveAndDisableInterrupts();

        // 1. Level 4
        SIZE_T PML4_IDX = (VirtAddr >> 39) & 0x1FF;
        U64 *PML4E = &PML4Virt[PML4_IDX];
        if(!(*PML4E & PAGE_PRESENT)) {
            Arch::RestoreInterrupts(_irq);
            return FALSE; // No mapping
        }
        U64 *PDPTVirt = HHDM_PhysToVirt(*PML4E & PAGE_ADDR_MASK);

        // 2. Level 3
        SIZE_T PDPT_IDX = (VirtAddr >> 30) & 0x1FF;
        U64 *PDPTe = &PDPTVirt[PDPT_IDX];
        if(!(*PDPTe & PAGE_PRESENT)) {
            Arch::RestoreInterrupts(_irq);
            return FALSE; 
        }
        if (*PDPTe & PAGE_PS) {
            Serial::Printf("[PAGING] ERROR: Unmap 4K inside 1G page! VA=%p\n", (void*)VirtAddr);
            Arch::RestoreInterrupts(_irq);
            return FALSE;
        }
        U64 *PDVirt = HHDM_PhysToVirt(*PDPTe & PAGE_ADDR_MASK);

        // 3. Level 2
        SIZE_T PD_IDX = (VirtAddr >> 21) & 0x1FF;
        U64 *PDe = &PDVirt[PD_IDX];
        if(!(*PDe & PAGE_PRESENT)) {
            Arch::RestoreInterrupts(_irq);
            return FALSE;
        }
        if (*PDe & PAGE_PS) {
            Serial::Printf("[PAGING] ERROR: Unmap 4K inside 2M page! VA=%p\n", (void*)VirtAddr);
            Arch::RestoreInterrupts(_irq);
            return FALSE;
        }
        U64 *PTVirt = HHDM_PhysToVirt(*PDe & PAGE_ADDR_MASK);

        // 4. Level 1 (actual PTE)
        SIZE_T PT_IDX = (VirtAddr >> 12) & 0x1FF;
        U64 *PTE = &PTVirt[PT_IDX];

        if(!(*PTE & PAGE_PRESENT)) {
            Arch::RestoreInterrupts(_irq);
            return FALSE; // Not mapped
        }

        // REMOVE MAPPING
        *PTE = 0;

        // TLB INVALID
        Arch::Invlpg(VirtAddr);

        // Try to free PT if empty
        bool pt_empty = true;
        for (size_t i = 0; i < 512; ++i) {
            if (PTVirt[i] & PAGE_PRESENT) { pt_empty = false; break; }
        }
        if (pt_empty) {
            // Free PT physical page and clear PD entry
            UPTR pt_phys = (UPTR)(*PDe & PAGE_ADDR_MASK);
            *PDe = 0;
            PageAlloc::PhysicalFreePages(pt_phys, 1);
        }

        // Try to free PD if empty
        bool pd_empty = true;
        for (size_t i = 0; i < 512; ++i) {
            if (PDVirt[i] & PAGE_PRESENT) { pd_empty = false; break; }
        }
        if (pd_empty) {
            UPTR pd_phys = (UPTR)(*PDPTe & PAGE_ADDR_MASK);
            *PDPTe = 0;
            PageAlloc::PhysicalFreePages(pd_phys, 1);
        }

        // Try to free PDPT if empty
        bool pdpt_empty = true;
        for (size_t i = 0; i < 512; ++i) {
            if (PDPTVirt[i] & PAGE_PRESENT) { pdpt_empty = false; break; }
        }
        if (pdpt_empty) {
            UPTR pdpt_phys = (UPTR)(*PML4E & PAGE_ADDR_MASK);
            *PML4E = 0;
            PageAlloc::PhysicalFreePages(pdpt_phys, 1);
        }

        // If the virtual address belongs to the kernel virtual pool, free it
        // from the virtual allocator. VirtualFreePages is a no-op if the
        // address is outside the pool, so this is safe to call.
        PageAlloc::VirtualFreePages((void*)VirtAddr, 1);

        Arch::RestoreInterrupts(_irq);
        return TRUE;
    }
    
}

// Provide compatibility wrapper in PageAlloc namespace so callers using
// PageAlloc::MapPages (earlier API) still link correctly.
    

namespace PageAlloc {
    BOOL MapPages(U64 *PML4Virt, UPTR PhysAddr, UPTR VirtAddr, SIZE_T Count, U64 Flags) {
        return Paging::MapPages(PML4Virt, PhysAddr, VirtAddr, Count, Flags);
    }

    BOOL UnMapPages(U64 *PML4Virt, UPTR VirtAddr) {
        return Paging::UnmapPage(PML4Virt, VirtAddr);
    }

    // Walk page tables to resolve a virtual address to a physical address.
    // Returns 0 if the virtual address is not mapped.
    UPTR GetPhysicalAddress(U64 *PML4Virt, UPTR VirtAddr) {
        LOCKRFLAGS _irq = Arch::SaveAndDisableInterrupts();

        // Level 4: PML4
        SIZE_T PML4_IDX = (VirtAddr >> 39) & 0x1FF;
        U64 PML4E = PML4Virt[PML4_IDX];
        if (!(PML4E & PAGE_PRESENT)) { Arch::RestoreInterrupts(_irq); return 0; }
        U64 *PDPTVirt = HHDM_PhysToVirt(PML4E & PAGE_ADDR_MASK);

        // Level 3: PDPT
        SIZE_T PDPT_IDX = (VirtAddr >> 30) & 0x1FF;
        U64 PDPTe = PDPTVirt[PDPT_IDX];
        if (!(PDPTe & PAGE_PRESENT)) { Arch::RestoreInterrupts(_irq); return 0; }
        if (PDPTe & PAGE_PS) {
            UPTR PhysBase = (UPTR)(PDPTe & PAGE_ADDR_MASK);
            UPTR Offset = VirtAddr & ((1ULL << 30) - 1);
            Arch::RestoreInterrupts(_irq);
            return PhysBase + Offset;
        }

        U64 *PDVirt = HHDM_PhysToVirt(PDPTe & PAGE_ADDR_MASK);

        // Level 2: PD
        SIZE_T PD_IDX = (VirtAddr >> 21) & 0x1FF;
        U64 PDe = PDVirt[PD_IDX];
        if (!(PDe & PAGE_PRESENT)) { Arch::RestoreInterrupts(_irq); return 0; }
        if (PDe & PAGE_PS) {
            UPTR PhysBase = (UPTR)(PDe & PAGE_ADDR_MASK);
            UPTR Offset = VirtAddr & ((1ULL << 21) - 1);
            Arch::RestoreInterrupts(_irq);
            return PhysBase + Offset;
        }

        U64 *PTVirt = HHDM_PhysToVirt(PDe & PAGE_ADDR_MASK);

        // Level 1: PT
        SIZE_T PT_IDX = (VirtAddr >> 12) & 0x1FF;
        U64 PTE = PTVirt[PT_IDX];
        if (!(PTE & PAGE_PRESENT)) { Arch::RestoreInterrupts(_irq); return 0; }

        UPTR PhysBase = (UPTR)(PTE & PAGE_ADDR_MASK);
        UPTR Offset = VirtAddr & (PAGE_SIZE - 1);
        Arch::RestoreInterrupts(_irq);
        return PhysBase + Offset;
    }

    // Update page flags for a mapped virtual address.
    // Returns TRUE on success, FALSE if the address is not mapped.
    BOOL SetFlags(U64 *PML4Virt, UPTR VirtAddr, U64 Flags) {
        LOCKRFLAGS _irq = Arch::SaveAndDisableInterrupts();

        // Level 4: PML4
        SIZE_T PML4_IDX = (VirtAddr >> 39) & 0x1FF;
        U64 PML4E = PML4Virt[PML4_IDX];
        if (!(PML4E & PAGE_PRESENT)) { Arch::RestoreInterrupts(_irq); return FALSE; }
        U64 *PDPTVirt = HHDM_PhysToVirt(PML4E & PAGE_ADDR_MASK);

        // Level 3: PDPT
        SIZE_T PDPT_IDX = (VirtAddr >> 30) & 0x1FF;
        U64 *PDPTe = &PDPTVirt[PDPT_IDX];
        if (!(*PDPTe & PAGE_PRESENT)) { Arch::RestoreInterrupts(_irq); return FALSE; }
        if (*PDPTe & PAGE_PS) {
            // This is a 1GB page; we can't set individual 4K page flags
            Arch::RestoreInterrupts(_irq);
            return FALSE;
        }

        U64 *PDVirt = HHDM_PhysToVirt(*PDPTe & PAGE_ADDR_MASK);

        // Level 2: PD
        SIZE_T PD_IDX = (VirtAddr >> 21) & 0x1FF;
        U64 *PDe = &PDVirt[PD_IDX];
        if (!(*PDe & PAGE_PRESENT)) { Arch::RestoreInterrupts(_irq); return FALSE; }
        if (*PDe & PAGE_PS) {
            // This is a 2MB page; we can't set individual 4K page flags
            Arch::RestoreInterrupts(_irq);
            return FALSE;
        }

        U64 *PTVirt = HHDM_PhysToVirt(*PDe & PAGE_ADDR_MASK);

        // Level 1: PT (actual PTE)
        SIZE_T PT_IDX = (VirtAddr >> 12) & 0x1FF;
        U64 *PTE = &PTVirt[PT_IDX];

        if (!(*PTE & PAGE_PRESENT)) { Arch::RestoreInterrupts(_irq); return FALSE; }

        // Preserve the physical address, update only the flags
        UPTR PhysAddr = *PTE & PAGE_ADDR_MASK;
        *PTE = PhysAddr | Flags;

        // Invalidate TLB entry for this address
        Arch::Invlpg(VirtAddr);

        Arch::RestoreInterrupts(_irq);
        return TRUE;
    }

    BOOL SetRegionFlags(U64 *PML4Virt, UPTR StartAddr, UPTR EndAddr, U64 Flags) {
        // Pastikan alamat diratakan (aligned) ke kelipatan 4096
        StartAddr &= ~0xFFFULL; 
        EndAddr = (EndAddr + 0xFFFULL) & ~0xFFFULL; 

        for (UPTR addr = StartAddr; addr < EndAddr; addr += 4096) {
            if (!SetFlags(PML4Virt, addr, Flags)) {
                Serial::Printf("[ERR] Gagal set flags di alamat %p\n", (void*)addr);
                return FALSE;
            }
        }
        return TRUE;
    }
}