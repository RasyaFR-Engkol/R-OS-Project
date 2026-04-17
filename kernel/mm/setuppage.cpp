/**
 * Copyright anjay
 */

#include "mm.hpp"
#include "../Include/serial.hpp"
#include "../Include/rossys.hpp"
#include "../mm/kmalloc/kmalloc.hpp"
#include "../../x86_64/tss.hpp"
#include <bootinfo.h>
#include <string.hpp>
#include <../kernel/mm/shm/shm.hpp>

// BUAT SEBUAH GLOBAL PML4 DISINI. Kita masih pake PML4 dari Boot.asm
// jadi sudah saatnya beralih ke sini dan beralih ke HHDM juga
U64 *KernelPML4;
UPTR KernelPML4Phys;

namespace Paging{
    // Virtual address of the kernel stack top after SwitchToKernelStack
    static UPTR KernelStackTopVirt = 0;

    void Initialize() {
        // Initialize allocators
        PageAlloc::Physical();
        PageAlloc::Virtual();

        // Reserve bootstrap page-tables so allocator won't hand out their frames
        constexpr UPTR EARLY_RESERVE_BASE = 0x00100000ULL;   // 1 MiB
        constexpr SIZE_T EARLY_RESERVE_PAGES = 512;          // cover first 2 MiB
        PageAlloc::PhysicalReserve(EARLY_RESERVE_BASE, EARLY_RESERVE_PAGES);
        // The boot stub maps the first 1 GiB identity using 2MiB pages.
        // Avoid touching boot tables here to keep higher-half code model clean.
        constexpr SIZE_T TWO_MB = 0x200000;

    // Allocate one physical page untuk PML4 baru (masih dalam identity-map)
    UPTR phys = PageAlloc::PhysicalAllocPages(1);
    KernelPML4Phys = phys;

    // Selagi identity mapping masih aktif kita bisa menulis PML4 lewat pointer
    KernelPML4 = (U64*)phys;

    // Nolkan seluruh entri terlebih dahulu
    for (size_t i = 0; i < 512; ++i) KernelPML4[i] = 0;

    // Copy entries from current PML4 (read CR3 physical address)
    U64 currentCr3 = 0;
    asm volatile("mov %%cr3, %0" : "=r"(currentCr3));
    U64 cr3Phys = currentCr3 & ~0xFFFULL;
    // Reserve bootstrap tables so allocator doesn't hand them out
    PageAlloc::PhysicalReserve((UPTR)cr3Phys, 1);
    U64* bootPML4 = (U64*)(UPTR)cr3Phys; // accessible via identity map
    // Reserve identity PDPT/PD used by the boot stub (slot 0)
    if (bootPML4[0] & PAGE_PRESENT) {
        U64 id_pdpt_phys = bootPML4[0] & ~0xFFFULL;
        PageAlloc::PhysicalReserve((UPTR)id_pdpt_phys, 1);
        U64* id_pdpt = (U64*)(UPTR)id_pdpt_phys;
        if (id_pdpt[0] & PAGE_PRESENT) {
            U64 id_pd_phys = id_pdpt[0] & ~0xFFFULL;
            PageAlloc::PhysicalReserve((UPTR)id_pd_phys, 1);
        }
    }
    for (size_t i = 0; i < 512; ++i) KernelPML4[i] = bootPML4[i];

    // Duplikasi entri low-half ke upper-half (256..511) supaya kernel punya
    // alias higher-half. Nantinya ketika kita pindah eksekusi ke alamat tinggi,
    // cukup gunakan entri atas ini dan bisa mengosongkan low-half per proses.
    for (size_t i = 0; i < 256; ++i) {
        if (KernelPML4[256 + i] == 0) {
            KernelPML4[256 + i] = bootPML4[i];
        }
    }

        // Build a HHDM (Higher Half Direct Map) sized from E820 information.
        constexpr UPTR HHDM_BASE = 0xffff800000000000ULL;
        const SIZE_T HHDM_PML4_INDEX = (SIZE_T)((HHDM_BASE >> 39) & 0x1ff);

        U64 total_usable_ram = 0;

        U64 max_phys = 0x40000000ULL; // default 1 GiB fallback
        if (const BootInfo* bi = BootInfoGet()) {
            if (bi->has_memmap) {
                for (U32 i = 0; i < bi->memmap.count; ++i) {
                    // Asumsi Tipe 1 adalah USABLE RAM (Standard E820 / Multiboot)
                    if (bi->memmap.regions[i].type == 1) { 
                        total_usable_ram += bi->memmap.regions[i].length;
                    }
                    
                    U64 end = bi->memmap.regions[i].base + bi->memmap.regions[i].length;
                    if (end > max_phys) max_phys = end;
                }
            }
            if (bi->has_framebuffer) {
                U64 fb_end = bi->framebuffer.address +
                    (U64)bi->framebuffer.pitch * (U64)bi->framebuffer.height;
                if (fb_end > max_phys) max_phys = fb_end;
            }
        }

        // Align up to 2 MiB so large pages cover full range.
    max_phys = (max_phys + (TWO_MB - 1)) & ~(U64)(TWO_MB - 1);
        if (max_phys == 0) max_phys = TWO_MB;

        const U64 ONE_GB = 0x40000000ULL;
        SIZE_T num_pdpt_entries = (SIZE_T)((max_phys + (ONE_GB - 1)) / ONE_GB);
        if (num_pdpt_entries == 0) num_pdpt_entries = 1;
        if (num_pdpt_entries > 512) num_pdpt_entries = 512;

        UPTR hhdm_pdpt_phys = PageAlloc::PhysicalAllocPages(1);
        U64* hhdm_pdpt = (U64*)hhdm_pdpt_phys; // identity mapped (allocator keeps <1 GiB)
        for (SIZE_T i = 0; i < 512; ++i) hhdm_pdpt[i] = 0;

        SIZE_T mapped_pdpt = 0;
        for (; mapped_pdpt < num_pdpt_entries; ++mapped_pdpt) {
            UPTR pd_phys = PageAlloc::PhysicalAllocPages(1);
            if (pd_phys == 0) break;
            U64* pd = (U64*)pd_phys;
            for (SIZE_T j = 0; j < 512; ++j) pd[j] = 0;

            U64 chunk_base = (U64)mapped_pdpt * ONE_GB;
            for (SIZE_T j = 0; j < 512; ++j) {
                U64 phys_base = chunk_base + (U64)j * (U64)TWO_MB;
                if (phys_base >= max_phys) break;
                pd[j] = phys_base | PAGE_PRESENT | PAGE_RW | PAGE_PS;
            }

            hhdm_pdpt[mapped_pdpt] = ((U64)pd_phys) | PAGE_PRESENT | PAGE_RW;
        }

        KernelPML4[HHDM_PML4_INDEX] = ((U64)hhdm_pdpt_phys) | PAGE_PRESENT | PAGE_RW;

    // Load new PML4 (CR3 requires physical address)
    DoCR3::Load((uint64_t*)KernelPML4Phys);

    // Enable NX (No-Execute) in EFER so the NX bit in page tables takes effect.
    // EFER.NXE is bit 11 (1 << 11).
    {
        U64 efer = Arch::MSR::ReadEFER();
        const U64 NXE_BIT = (1ULL << 11);
        if (!(efer & NXE_BIT)) {
            Arch::MSR::WriteEFER(efer | NXE_BIT);
            Serial::Write("[ROS] EFER.NXE enabled\n");
        } else {
            Serial::Write("[ROS] EFER.NXE already set\n");
        }
    }

    // Inisialisasi PAT (Page Attribute Table)
    // Kita set Entry 4 (PA4) menjadi Write-Combining (0x01).
    // Indeks 4 dipilih hardware jika: PAT=1, PCD=0, PWT=0.
    {
        U32 low, high;
        asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0x277));
        U64 pat = ((U64)high << 32) | low;
        pat &= ~(0xFFULL << 32); // Kosongkan PA4 (bits 32-39)
        pat |= (0x01ULL << 32);  // Isi PA4 dengan 0x01 (WC)
        low = (U32)pat;
        high = (U32)(pat >> 32);
        asm volatile("wrmsr" : : "a"(low), "d"(high), "c"(0x277));
        Printk::Write(Printk::Level::LOG_INFO, "PAT initialized: PA4 set to Write-Combining (0x01)\n");
    }

    // After loading CR3, the higher-half direct map is active.
    // Update KernelPML4 to point to the HHDM-mapped virtual address
    // so future dereferences use the higher-half view.
    KernelPML4 = (U64*)HHDM_PhysToVirt(KernelPML4Phys);

        Serial::Write("[ROS] Paging Activated\n");
        // print physical CR3 and its HHDM virtual mapping
        void* hhdm_virt = (void*)(HHDM_BASE + KernelPML4Phys);
        Serial::Printf("[ROS] CR3 phys: %p hhdm_virt: %p identity_virt: %p mapped=%u GiB\n",
            (void*)KernelPML4Phys, hhdm_virt, (void*)KernelPML4, (unsigned)mapped_pdpt);

        // Initialize kmalloc after paging is active so it can use the new allocator and HHDM mappings.

        // ==========================================
        // DYNAMIC KMALLOC SIZING
        // ==========================================
        // We allocate
        U64 kmalloc_bytes = total_usable_ram / 20; 

        const U64 MIN_KMALLOC = 16ULL * 1024 * 1024;
        const U64 MAX_KMALLOC = 256ULL * 1024 * 1024;

        if (kmalloc_bytes < MIN_KMALLOC) kmalloc_bytes = MIN_KMALLOC;
        if (kmalloc_bytes > MAX_KMALLOC) kmalloc_bytes = MAX_KMALLOC;

        SIZE_T kmalloc_pages = (SIZE_T)(kmalloc_bytes / PAGE_SIZE);

        Serial::Printf("[ROS] Dynamic Kmalloc: total RAM ~%llu MB, reserving %llu MB (%llu pages) for heap\n", 
                    total_usable_ram / (1024*1024), 
                    kmalloc_bytes / (1024*1024), 
                    (unsigned long long)kmalloc_pages);

        // Panggil init dengan jumlah pages yang udah dinamis!
        Kmalloc::Init(kmalloc_pages);

        // Initialize DMA pool now that physical/virtual allocators and kmalloc
        // are available and HHDM mapping is active. This prepares the dedicated
        // DMA pool so callers don't need to initialize it lazily.
        Serial::Write("[DMA] InitializeDMA from setuppage\n");
        PageAlloc::DMAAlloc::InitializeDMA();

        // Relocate GDT into high memory before disabling the low-half
        RelocateGDTToHigh();
        // Install user-space (ring3) code/data selectors into the GDT so
        // processes can use user-mode selectors when we start creating
        // user contexts.
        AddUserGDTEntries();

        // Switch to a fresh kernel stack in HHDM (8 pages = 32 KiB) and then
        // continue initialization on the new stack in AfterStackSwitch().
        SwitchToKernelStack(16);
    }

    void RelocateToHigherHalf() {
        static bool already = false;
        if (already) return;
        already = true;

        constexpr UPTR HHDM_BASE = 0xffff800000000000ULL;
        UPTR new_rsp;
        UPTR new_rbp;
        asm volatile("mov %%rsp, %0" : "=r"(new_rsp));
        asm volatile("mov %%rbp, %0" : "=r"(new_rbp));
        new_rsp += HHDM_BASE;
        new_rbp += HHDM_BASE;

        asm volatile(
            "mov %0, %%rsp\n\t"
            "mov %1, %%rbp\n\t"
            "lea 1f(%%rip), %%rax\n\t"
            "add %2, %%rax\n\t"
            "jmp *%%rax\n"
            "1:\n"
            :
            : "r"(new_rsp), "r"(new_rbp), "r"(HHDM_BASE)
            : "rax"
        );
    }

    // Read current GDTR, copy GDT to HHDM-backed memory, and load new GDTR.
    void RelocateGDTToHigh() {
        struct GDTR { U16 limit; U64 base; } __attribute__((packed));
        GDTR gdtr;
        asm volatile("sgdt %0" : "=m"(gdtr));
        if (gdtr.base >= HHDM_BASE) {
            // Already high
            Serial::Write("[ROS] GDT already high\n");
            return;
        }
        SIZE_T size = (SIZE_T)gdtr.limit + 1;
        // Allocate physical pages to hold the GDT, use HHDM alias
        SIZE_T pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
        UPTR phys = PageAlloc::PhysicalAllocPages(pages);
        U8* dst = (U8*)HHDM_PhysToVirt(phys);
        String::Memcpy(dst, (void*)(UPTR)gdtr.base, size);
        GDTR newgdtr { (U16)(size - 1), (U64)(UPTR)dst };
        asm volatile("lgdt %0" :: "m"(newgdtr));
        Serial::Printf("[ROS] GDT moved: old=%p new=%p size=%u\n", (void*)(UPTR)gdtr.base, dst, (unsigned)size);
    }

    // Continuation executed on the new stack
    static void SwitchResumeThunk();
    static void AfterStackSwitch(void*) {
        // Initialize TSS now that we've relocated the GDT and switched to the HHDM kernel stack.
        if (KernelStackTopVirt) {
            TSS::Init(KernelStackTopVirt);
        }

        // Optional: Audit mappings for sanity
        AuditMappings();
        // Finally, disable low-half mappings (0..255) now that everything is high
        DisableLowHalf();
    }

    ABI_C void Arch_SwitchStackResume2(void* new_sp, void* first_ip, void* second_ip);

    // Allocate N pages for a new kernel stack in HHDM and switch to it.
    void SwitchToKernelStack(SIZE_T pages) {
        if (pages == 0) pages = 8;
        static bool done = false;
        if (done) return;
        done = true;
        UPTR phys = PageAlloc::PhysicalAllocPages(pages);
        U8* base = (U8*)HHDM_PhysToVirt(phys);
        U8* top = base + pages * PAGE_SIZE;
        // Record the kernel stack top (virtual in HHDM) so TSS can point RSP0 here
        KernelStackTopVirt = (UPTR)top;
        Serial::Printf("[ROS] Switching to kernel stack: base=%p top=%p pages=%u\n", base, top, (unsigned)pages);
    // Capture caller's return address so we can return there after
    // running post-switch tasks on the new stack.
    void* retaddr = __builtin_return_address(0);
    Arch_SwitchStackResume2(top, (void*)SwitchResumeThunk, retaddr);
    // Unreachable logically; control has moved to new stack and will
    // return directly to our caller after SwitchResumeThunk completes.
    return;
    }

    static void SwitchResumeThunk() {
        // Run post-switch tasks on the new stack, then return to caller
        AfterStackSwitch(nullptr);
    }

    void DisableLowHalf() {
        // Zero PML4 entries 0..255
        SIZE_T cleared = 0;
        for (SIZE_T i = 0; i < 256; ++i) {
            if (KernelPML4[i]) { KernelPML4[i] = 0; ++cleared; }
        }
        // Reload CR3 to flush TLB
        DoCR3::Load((uint64_t*)KernelPML4Phys);
        Serial::Printf("[ROS] Low-half disabled, cleared %u PML4 entries\n", (unsigned)cleared);
    }

    void AuditMappings() {
        // Count non-zero entries in low and high halves
        SIZE_T low = 0, high = 0;
        for (SIZE_T i = 0; i < 256; ++i) if (KernelPML4[i]) ++low;
        for (SIZE_T i = 256; i < 512; ++i) if (KernelPML4[i]) ++high;
        const SIZE_T HHDM_INDEX = (HHDM_BASE >> 39) & 0x1FF;
        BOOL hhdm_present = (KernelPML4[HHDM_INDEX] & PAGE_PRESENT) != 0;
        Serial::Printf("[ROS] Audit: PML4 low=%u high=%u HHDM[idx=%u]=%s\n",
            (unsigned)low, (unsigned)high, (unsigned)HHDM_INDEX, hhdm_present ? "yes" : "no");
    }
}