#include <rosval.h>
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

                PDPTVirt = HHDM_PhysToVirt(NewPDPTPhys);
                String::Memset(PDPTVirt, 0, PAGE_SIZE);

                *PML4E = NewPDPTPhys | PAGE_PRESENT | PAGE_RW | PAGE_USER;
            } else {
                PDPTVirt = HHDM_PhysToVirt(*PML4E & PAGE_ADDR_MASK);
            }

            // 2. Level 3: PDPT
            SIZE_T PDPTIDX = (CurrentVirt >> 30) & 0x1FF;
            U64* PDPTe = &PDPTVirt[PDPTIDX];

            U64* PDVirt;
            if(!(*PDPTe & PAGE_PRESENT)) {
                UPTR NewPDPhys = PageAlloc::PhysicalAllocPages(1);
                if(NewPDPhys == 0) { Arch::RestoreInterrupts(_irq); return FALSE; }

                PDVirt = HHDM_PhysToVirt(NewPDPhys);
                String::Memset(PDVirt, 0, PAGE_SIZE);
                *PDPTe = NewPDPhys | PAGE_PRESENT | PAGE_RW | PAGE_USER;
            } else {
                if (*PDPTe & PAGE_PS) {
                    Serial::Printf("[PAGING] ERROR: Try mapping 4K up to 1G page! VA=%p", (void*)CurrentVirt);
                    return FALSE; // Gunakan FALSE
                }
                PDVirt = HHDM_PhysToVirt(*PDPTe & PAGE_ADDR_MASK);
            }

            // 3. Level 2: PD
            SIZE_T PDIDX = (CurrentVirt >> 21) & 0x1FF;
            U64 *PDe = &PDVirt[PDIDX];

            U64 *PTVirt;
            if(!(*PDe & PAGE_PRESENT)) {
                UPTR NewPTPhys = PageAlloc::PhysicalAllocPages(1);
                if(NewPTPhys == 0) { Arch::RestoreInterrupts(_irq); return FALSE; }

                PTVirt = HHDM_PhysToVirt(NewPTPhys);
                String::Memset(PTVirt, 0, PAGE_SIZE);
                *PDe = NewPTPhys | PAGE_PRESENT | PAGE_RW | PAGE_USER;
            } else {
                if(*PDe & PAGE_PS) {
                    Serial::Printf("[PAGING] ERROR: Try mapping 4K up to 2M page! VA=%p", (void*)CurrentVirt);
                    Arch::RestoreInterrupts(_irq);
                    return FALSE;
                }
                PTVirt = HHDM_PhysToVirt(*PDe & PAGE_ADDR_MASK);
            }

            // 4. Level 1: PT (PAGE_TABLE)
            SIZE_T PTIDX = (CurrentVirt >> 12) & 0x01FF;
            U64 *PTE = &PTVirt[PTIDX];

            *PTE = CurrentPhys | Flags;

            Arch::Invlpg(CurrentVirt);
        }

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
}