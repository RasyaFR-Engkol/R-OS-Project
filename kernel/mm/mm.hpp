#pragma once

#include <rosval.h>

// Paging Size
#define PAGE_SIZE 0x1000

// Status paging

// Paging flags (x86-64)
#define PAGE_PRESENT   0x1      // bit 0
#define PAGE_RW        0x2      // bit 1: read/write
#define PAGE_USER      0x4      // bit 2: user/supervisor
#define PAGE_PWT       0x8      // bit 3: page-level write-through
#define PAGE_PCD       0x10     // bit 4: page-level cache disable
#define PAGE_ACCESSED  0x20     // bit 5: accessed
#define PAGE_DIRTY     0x40     // bit 6: dirty
#define PAGE_PS        0x80     // bit 7: huge page (2 MiB / 1 GiB)
#define PAGE_GLOBAL    0x100    // bit 8: global page
#define PAGE_NX        (1ULL << 63) // bit 63: no-execute (EFER.NXE harus di-set)

// --- Tambahkan konstanta ini ---
constexpr UPTR HHDM_BASE = 0xffff800000000000ULL;
// Ambil 44 bit alamat (abaikan 12 bit offset + 9 bit PT + ... dst)
constexpr U64 PAGE_ADDR_MASK = 0x000FFFFFFFFFF000ULL; 

extern UPTR KernelPML4Phys;
extern U64 *KernelPML4;

// --- Tambahkan helper Arch::Invlpg ---
namespace Arch {
    inline void Invlpg(UPTR vaddr) {
        asm volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
    }
}

// Paging setup & control
namespace Paging {
    void Initialize();
    // Legacy helper (no-op now); kept for compatibility
    void RelocateToHigherHalf();

    // New helpers
    void RelocateGDTToHigh();
    // Install user-mode (ring3) code/data segment descriptors into the
    // current GDT. This adds selectors for user code and user data so the
    // kernel can switch to userland segments when creating processes.
    void AddUserGDTEntries();
    void SwitchToKernelStack(SIZE_T pages /* default 8 pages allocated */);
    void DisableLowHalf();
    void AuditMappings();
}

// Virtual & Physical allocator
namespace PageAlloc{
    // Initialize virtual allocator (bitmap-backed)
    void Virtual();
    void Physical();

    // Allocate 'count' contiguous pages from the virtual pool. Returns virtual
    // address on success or nullptr on failure.
    void* VirtualAllocPages(SIZE_T count);

    // Free 'count' pages previously allocated at 'addr'. 'addr' must be an
    // address previously returned by VirtualAllocPages.
    void VirtualFreePages(void* addr, SIZE_T count);

    // Physical allocator API: allocate/free physical frames (returns physical
    // address as UPTR). Adjust PHYS_BASE/PHYS_FRAMES in implementation as
    // needed for your platform.
    UPTR PhysicalAllocPages(SIZE_T count);
    void PhysicalFreePages(UPTR addr, SIZE_T count);
    // Mark a physical range as used/reserved so allocator won't return it.
    void PhysicalReserve(UPTR addr, SIZE_T count);
    // Low-memory (below 1 MiB) helpers for special allocations like trampoline
    UPTR PhysicalAllocLowPages(SIZE_T count);
    void PhysicalFreeLowPages(UPTR addr, SIZE_T count);
    void PhysicalReserveLow(UPTR addr, SIZE_T count);

    UPTR GetPhysicalAddress(U64 *PML4Virt, UPTR VirtAddr);
    BOOL MapPages(U64 *PML4Virt, UPTR PhysAddr, UPTR VirtAddr, SIZE_T Count, U64 Flags);
    BOOL UnMapPages(U64 *PML4Virt, UPTR VirtAddr);
    BOOL SetFlags(U64 *PML4Virt, UPTR VirtAddr, U64 Flags);
    // Safe user/kernel copy helpers. UserPML4 is the PML4 virtual pointer
    // for the user's address space (HHDM_PhysToVirt(cr3_phys)). These routines
    // validate that pages are present and user-accessible before copying.
    BOOL CopyFromUser(U64 *UserPML4, void* dstKernel, const void* srcUser, SIZE_T len);
    BOOL CopyToUser(U64 *UserPML4, void* dstUser, const void* srcKernel, SIZE_T len);
    namespace DMAAlloc{
        struct DMABuffer{
            public:
            UPTR PhysAddr;
            UPTR VirtAddr;
            SIZE_T Size;

            private:
            SIZE_T Pages;
            SIZE_T GuardPages;
            SIZE_T TotalPages; // usable pages + guard pages (for pool bookkeeping)
            SIZE_T RequestedBytes; // original caller size (bytes) for diagnostics
            SIZE_T FirstIndex;

            friend DMABuffer* AllocateDMAPages(SIZE_T);
            friend DMABuffer *AllocateDMABytes(SIZE_T bytes);
            friend void FreeDMABuffer(DMABuffer*);
            friend SIZE_T guard_bytes(const DMABuffer* buf);
            friend void fill_guard_region(DMABuffer* buf);
            friend bool guard_region_intact(const DMABuffer* buf);
        };
        void InitializeDMA();
        DMABuffer *AllocateDMAPages(SIZE_T count);
        // Convenience: allocate by byte size (rounded up to pages)
        DMABuffer *AllocateDMABytes(SIZE_T bytes);
        void FreeDMAPages(UPTR addr, SIZE_T count);
        // Prefer this overload when AllocateDMAPages() returned a DMABuffer*;
        // frees both the pages (bitmap) and the metadata struct.
        void FreeDMABuffer(DMABuffer* b);

        // Debug/statistics helper for runtime inspection
        void GetStats(SIZE_T *poolPages, SIZE_T *freePages, SIZE_T *allocCalls,
                      SIZE_T *fallbacks, SIZE_T *failed, SIZE_T *freed);

        SIZE_T guard_bytes(const PageAlloc::DMAAlloc::DMABuffer* buf);
        void fill_guard_region(PageAlloc::DMAAlloc::DMABuffer* buf);
        bool guard_region_intact(const PageAlloc::DMAAlloc::DMABuffer* buf);
    }

}

namespace DoCR3 {
    static inline void Load(uint64_t *cr3) {
        asm volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
    }

    static inline uint64_t* GetCurrentCR3() {
        uint64_t cr3;
        asm volatile("mov %%cr3, %0" : "=r"(cr3));
        return (uint64_t*)cr3;
    }
}

static inline U64 *HHDM_PhysToVirt(UPTR Phys){
    return (U64*)(Phys + HHDM_BASE);
}

// Menarik untuk menambahkan ini asli
typedef U64 PFLAGS;