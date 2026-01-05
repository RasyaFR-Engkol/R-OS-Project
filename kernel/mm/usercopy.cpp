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
#include <logging.hpp>

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

// Diagnostic helper: print the page-table entries for a virtual address
// and return FALSE if the address would not be considered a valid user
// mapping by vaddr_to_phys_page. Useful for runtime debugging.
BOOL PageAlloc::DumpVaddrMapping(U64 *UserPML4, UPTR vaddr) {
    if (!UserPML4) return FALSE;

    SIZE_T pml4i = (vaddr >> 39) & 0x1FF;
    SIZE_T pdpti = (vaddr >> 30) & 0x1FF;
    SIZE_T pdi = (vaddr >> 21) & 0x1FF;
    SIZE_T pti = (vaddr >> 12) & 0x1FF;

    U64 pml4e = UserPML4[pml4i];
    Printk::Write(Printk::Level::LOG_ERR, "[USERCOPY] Dump: vaddr=%p indices PML4=%llu PDPT=%llu PD=%llu PT=%llu\n",
                  (void*)vaddr, (unsigned long long)pml4i, (unsigned long long)pdpti,
                  (unsigned long long)pdi, (unsigned long long)pti);
    Printk::Write(Printk::Level::LOG_ERR, "[USERCOPY] PML4[%llu]=0x%llx\n", (unsigned long long)pml4i, (unsigned long long)pml4e);

    if (!(pml4e & PAGE_PRESENT)) {
        Printk::Write(Printk::Level::LOG_ERR, "[USERCOPY] fail: PML4 entry not present\n");
        return FALSE;
    }
    if (!(pml4e & PAGE_USER)) {
        Printk::Write(Printk::Level::LOG_ERR, "[USERCOPY] fail: PML4 entry not user-accessible\n");
        return FALSE;
    }

    U64 *pdpt = (U64*)HHDM_PhysToVirt(pml4e & PAGE_ADDR_MASK);
    U64 pdpte = pdpt[pdpti];
    Printk::Write(Printk::Level::LOG_ERR, "[USERCOPY] PDPT[%llu]=0x%llx\n", (unsigned long long)pdpti, (unsigned long long)pdpte);
    if (!(pdpte & PAGE_PRESENT)) {
        Printk::Write(Printk::Level::LOG_ERR, "[USERCOPY] fail: PDPT entry not present\n");
        return FALSE;
    }
    if (!(pdpte & PAGE_USER)) {
        Printk::Write(Printk::Level::LOG_ERR, "[USERCOPY] fail: PDPT entry not user-accessible\n");
        return FALSE;
    }

    if (pdpte & PAGE_PS) {
        Printk::Write(Printk::Level::LOG_ERR, "[USERCOPY] maps with 1GiB page\n");
        return TRUE;
    }

    U64 *pd = (U64*)HHDM_PhysToVirt(pdpte & PAGE_ADDR_MASK);
    U64 pde = pd[pdi];
    Printk::Write(Printk::Level::LOG_ERR, "[USERCOPY] PD[%llu]=0x%llx\n", (unsigned long long)pdi, (unsigned long long)pde);
    if (!(pde & PAGE_PRESENT)) {
        Printk::Write(Printk::Level::LOG_ERR, "[USERCOPY] fail: PD entry not present\n");
        return FALSE;
    }
    if (!(pde & PAGE_USER)) {
        Printk::Write(Printk::Level::LOG_ERR, "[USERCOPY] fail: PD entry not user-accessible\n");
        return FALSE;
    }

    if (pde & PAGE_PS) {
        Printk::Write(Printk::Level::LOG_ERR, "[USERCOPY] maps with 2MiB page\n");
        return TRUE;
    }

    U64 *pt = (U64*)HHDM_PhysToVirt(pde & PAGE_ADDR_MASK);
    U64 pte = pt[pti];
    Printk::Write(Printk::Level::LOG_ERR, "[USERCOPY] PT[%llu]=0x%llx\n", (unsigned long long)pti, (unsigned long long)pte);
    if (!(pte & PAGE_PRESENT)) {
        Printk::Write(Printk::Level::LOG_ERR, "[USERCOPY] fail: PT entry not present\n");
        return FALSE;
    }
    if (!(pte & PAGE_USER)) {
        Printk::Write(Printk::Level::LOG_ERR, "[USERCOPY] fail: PT entry not user-accessible\n");
        return FALSE;
    }

    Printk::Write(Printk::Level::LOG_ERR, "[USERCOPY] success: mapping present and user-accessible\n");
    return TRUE;
}

BOOL PageAlloc::DumpVaddrRange(U64 *UserPML4, UPTR vaddr, SIZE_T len) {
    if (!UserPML4) return FALSE;
    if (len == 0) return TRUE;

    UPTR start = vaddr & ~(PAGE_SIZE - 1);
    UPTR end = vaddr + len;
    for (UPTR a = start; a < end; a += PAGE_SIZE) {
        if (!vaddr_to_phys_page(UserPML4, a, nullptr, nullptr, nullptr)) {
            Printk::Write(Printk::Level::LOG_ERR, "[USERCOPY] DumpRange: mapping failed at vaddr=%p\n", (void*)a);
            // print the detailed mapping for this failing address
            DumpVaddrMapping(UserPML4, a);
            return FALSE;
        }
    }
    Printk::Write(Printk::Level::LOG_ERR, "[USERCOPY] DumpRange: whole range present/user-accessible\n");
    return TRUE;
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
