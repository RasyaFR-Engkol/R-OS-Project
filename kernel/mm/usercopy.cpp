/* Safe copy helpers between user and kernel address spaces.
 * These functions walk the provided user PML4 (virtual pointer in HHDM)
 * and translate virtual addresses to physical frames. They require that the
 * pages are present and have the PAGE_USER bit set. They support 4K/2M/1G
 * mappings.
 */

#include "usercopy.hpp"
#include "mm.hpp"
#include <string.hpp>
#include <serial.hpp>
#include <rossys.hpp>

using namespace Serial;

namespace {
    // Walk page tables in PML4Virt and return the physical base of the page
    // that covers `vaddr`. On success returns TRUE and fills phys_base and
    // page_size (4096/2MiB/1GiB). Also returns the flags word of the final
    // entry via out_flags if non-null.
    bool vaddr_to_phys_page(U64 *PML4Virt, UPTR vaddr, UPTR *phys_base, SIZE_T *page_size, U64 *out_flags) {
        // Level 4
        SIZE_T pml4i = (vaddr >> 39) & 0x1FF;
        U64 pml4e = PML4Virt[pml4i];
        if (!(pml4e & PAGE_PRESENT)) return false;
        if (!(pml4e & PAGE_USER)) return false;

        U64 *pdpt = (U64*)HHDM_PhysToVirt(pml4e & PAGE_ADDR_MASK);
        SIZE_T pdpti = (vaddr >> 30) & 0x1FF;
        U64 pdpte = pdpt[pdpti];
        if (!(pdpte & PAGE_PRESENT)) return false;
        if (!(pdpte & PAGE_USER)) return false;
        // 1 GiB page
        if (pdpte & PAGE_PS) {
            UPTR base = (UPTR)(pdpte & PAGE_ADDR_MASK);
            if (out_flags) *out_flags = pdpte;
            if (page_size) *page_size = 0x40000000ULL; // 1 GiB
            if (phys_base) *phys_base = base + (vaddr & 0x3FFFFFFFULL);
            return true;
        }

        U64 *pd = (U64*)HHDM_PhysToVirt(pdpte & PAGE_ADDR_MASK);
        SIZE_T pdi = (vaddr >> 21) & 0x1FF;
        U64 pde = pd[pdi];
        if (!(pde & PAGE_PRESENT)) return false;
        if (!(pde & PAGE_USER)) return false;
        // 2 MiB page
        if (pde & PAGE_PS) {
            UPTR base = (UPTR)(pde & PAGE_ADDR_MASK);
            if (out_flags) *out_flags = pde;
            if (page_size) *page_size = 0x200000ULL; // 2 MiB
            if (phys_base) *phys_base = base + (vaddr & 0x1FFFFFULL);
            return true;
        }

        U64 *pt = (U64*)HHDM_PhysToVirt(pde & PAGE_ADDR_MASK);
        SIZE_T pti = (vaddr >> 12) & 0x1FF;
        U64 pte = pt[pti];
        if (!(pte & PAGE_PRESENT)) return false;
        if (!(pte & PAGE_USER)) return false;
        UPTR base = (UPTR)(pte & PAGE_ADDR_MASK);
        if (out_flags) *out_flags = pte;
        if (page_size) *page_size = PAGE_SIZE;
        if (phys_base) *phys_base = base + (vaddr & (PAGE_SIZE - 1));
        return true;
    }
}

namespace PageAlloc {

    BOOL CopyFromUser(U64 *UserPML4, void* dstKernel, const void* srcUser, SIZE_T len) {
        if (len == 0) return TRUE;
        if (UserPML4 == nullptr || dstKernel == nullptr || srcUser == nullptr) return FALSE;

        LOCKRFLAGS _irq = Arch::SaveAndDisableInterrupts();

        UPTR dst = (UPTR)dstKernel;
        UPTR src = (UPTR)srcUser;
        SIZE_T remaining = len;

        while (remaining > 0) {
            UPTR phys_base = 0;
            SIZE_T page_size = 0;
            if (!vaddr_to_phys_page(UserPML4, src, &phys_base, &page_size, nullptr)) {
                Arch::RestoreInterrupts(_irq);
                return FALSE;
            }

            UPTR page_start = src & ~(page_size - 1);
            SIZE_T offset_in_page = src - page_start;
            SIZE_T avail = (SIZE_T)(page_size - offset_in_page);
            SIZE_T chunk = (avail < remaining) ? avail : remaining;

            void* from = (void*)HHDM_PhysToVirt(phys_base);
            void* to   = (void*)dst;
            String::Memcpy(to, from, chunk);

            src += chunk;
            dst += chunk;
            remaining -= chunk;
        }

        Arch::RestoreInterrupts(_irq);
        return TRUE;
    }

    BOOL CopyToUser(U64 *UserPML4, void* dstUser, const void* srcKernel, SIZE_T len) {
        if (len == 0) return TRUE;
        if (UserPML4 == nullptr || dstUser == nullptr || srcKernel == nullptr) return FALSE;

    LOCKRFLAGS _irq = Arch::SaveAndDisableInterrupts();

        UPTR dst = (UPTR)dstUser;
        UPTR src = (UPTR)srcKernel;
        SIZE_T remaining = len;

        while (remaining > 0) {
            UPTR phys_base = 0;
            SIZE_T page_size = 0;
            if (!vaddr_to_phys_page(UserPML4, dst, &phys_base, &page_size, nullptr)) {
                Arch::RestoreInterrupts(_irq);
                return FALSE;
            }

            UPTR page_start = dst & ~(page_size - 1);
            SIZE_T offset_in_page = dst - page_start;
            SIZE_T avail = (SIZE_T)(page_size - offset_in_page);
            SIZE_T chunk = (avail < remaining) ? avail : remaining;

            void* to = (void*)HHDM_PhysToVirt(phys_base);
            void* from = (void*)src;
            String::Memcpy(to, from, chunk);

            dst += chunk;
            src += chunk;
            remaining -= chunk;
        }

        Arch::RestoreInterrupts(_irq);
        return TRUE;
    }

}
