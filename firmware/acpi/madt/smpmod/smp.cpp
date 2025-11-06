#include <rossys.hpp>
#include <rosval.h>
#include "../../acpi.hpp"
#include "../madt.hpp"
#include "port.hpp"
#include "smp.hpp"
#include <mm.hpp>
#include <logging.hpp>

extern "C" void ap_main_entry(U32 apic_id); // Provided below stub; user replace later
extern "C" const U8 _binary_build_firmware_acpi_madt_smpmod_rmpmlmtramp_bin_start[];
extern "C" const U8 _binary_build_firmware_acpi_madt_smpmod_rmpmlmtramp_bin_end[];

namespace {
    constexpr UPTR TRAMPOLINE_PHYS = 0x8000; // must match ORG in ASM
    constexpr U8 TRAMPOLINE_VECTOR = TRAMPOLINE_PHYS >> 12; // SIPI vector

    constexpr U32 TRAMP_META_SIGNATURE = 0x54524D50u; // 'TRMP'

    struct TrampolineMetadata {
        U32 signature;
        U32 version;
        U32 pml4_offset;
        U32 gdtptr_offset;
        U32 ap_stack_offset;
        U32 ap_main_offset;
        U32 apic_offset;
        U32 image_size;
    } __attribute__((packed));

    // Simple memory copy
    void phys_memcpy(UPTR phys_dst, const void* src, SIZE_T len){
        volatile U8* d = (volatile U8*)(phys_dst + HHDM_BASE); // assume HHDM maps phys 1:1 offset
        const U8* s = (const U8*)src;
        for (SIZE_T i=0;i<len;++i) d[i]=s[i];
    }
    void phys_memset(UPTR phys_dst, U8 value, SIZE_T len){
        volatile U8* d = (volatile U8*)(phys_dst + HHDM_BASE);
        for (SIZE_T i = 0; i < len; ++i) d[i] = value;
    }
    void phys_write_u32(UPTR phys_base, SIZE_T off, U32 val){
        volatile U32* p = (volatile U32*)(phys_base + off + HHDM_BASE);
        *p = val;
    }
    void phys_write_u64(UPTR phys_base, SIZE_T off, U64 val){
        volatile U64* p = (volatile U64*)(phys_base + off + HHDM_BASE);
        *p = val;
    }

    UPTR AllocatePageForTrampoline() {
        UPTR phys = PageAlloc::PhysicalAllocLowPages(1);
        if (phys) return phys;

        phys = PageAlloc::PhysicalAllocPages(1);
        if (phys && phys >= 0x100000000ULL) {
            PageAlloc::PhysicalFreePages(phys, 1);
            phys = 0;
        }
        return phys;
    }

    volatile U32 g_ApReadyCount = 0;
    U32 g_ApReadyTarget = 0;

    struct ApStackRecord {
        U32 apic_id;
        UPTR stack_phys;
        UPTR stack_top_virt;
    };

    ApStackRecord g_ApStacks[MAX_CPU_COUNT];
    U32 g_ApStackCount = 0;

    inline U32 AtomicFetchAdd(volatile U32* target, U32 value) {
        asm volatile ("lock xaddl %0, %1" : "+r"(value), "+m"(*target) :: "memory");
        return value;
    }

    void WaitForApBarrier() {
        if (!g_ApReadyTarget) {
            return;
        }

        using namespace Printk;
        Write(Level::LOG_INFO, "SMP: waiting for %u AP(s) to report ready\n", g_ApReadyTarget);

        constexpr U32 timeout_ms = 5000;
        U32 waited = 0;
        while (g_ApReadyCount < g_ApReadyTarget && waited < timeout_ms) {
            Arch::Time::Sleep(1);
            ++waited;
        }

        if (g_ApReadyCount < g_ApReadyTarget) {
            Write(Level::LOG_WARNING,
                  "SMP: barrier timed out (%u/%u APs ready)\n",
                  g_ApReadyCount, g_ApReadyTarget);
        } else {
            Write(Level::LOG_INFO,
                  "SMP: all APs reported ready (%u/%u)\n",
                  g_ApReadyCount, g_ApReadyTarget);
        }
    }

    UPTR EnsureTrampolinePML4() {
        static UPTR trampoline_pml4_phys = 0;
        if (trampoline_pml4_phys) return trampoline_pml4_phys;

        UPTR pml4_phys = AllocatePageForTrampoline();
        UPTR pdpt_phys = AllocatePageForTrampoline();
        UPTR pd_phys   = AllocatePageForTrampoline();
        if (!pml4_phys || !pdpt_phys || !pd_phys) {
            if (pml4_phys) PageAlloc::PhysicalFreePages(pml4_phys, 1);
            if (pdpt_phys) PageAlloc::PhysicalFreePages(pdpt_phys, 1);
            if (pd_phys) PageAlloc::PhysicalFreePages(pd_phys, 1);
            return 0;
        }

        U64* pml4 = (U64*)HHDM_PhysToVirt(pml4_phys);
        U64* pdpt = (U64*)HHDM_PhysToVirt(pdpt_phys);
        U64* pd   = (U64*)HHDM_PhysToVirt(pd_phys);

        for (SIZE_T i = 0; i < 512; ++i) {
            pml4[i] = 0;
            pdpt[i] = 0;
            pd[i] = 0;
        }

        // Identity map the first 1GiB using 2MiB large pages so stacks and low data stay accessible
        for (SIZE_T i = 0; i < 512; ++i) {
            pd[i] = (static_cast<U64>(i) << 21) | PAGE_PRESENT | PAGE_RW | PAGE_PS;
        }
        pdpt[0] = ((U64)pd_phys) | PAGE_PRESENT | PAGE_RW;
        pml4[0] = ((U64)pdpt_phys) | PAGE_PRESENT | PAGE_RW;

        // Copy high-half entries from kernel PML4 so higher-half kernel remains mapped
        for (SIZE_T i = 256; i < 512; ++i) {
            pml4[i] = KernelPML4[i];
        }

        Printk::Write(Printk::Level::LOG_INFO,
                      "SMP: trampoline PML4 built at phys 0x%p (PDPT 0x%p, PD 0x%p)\n",
                      (void*)pml4_phys, (void*)pdpt_phys, (void*)pd_phys);

        trampoline_pml4_phys = pml4_phys;
        return trampoline_pml4_phys;
    }
}

namespace ACPI{
    namespace LAPIC{
        namespace SMP{
            using namespace ACPI;
            VOID WaitForIPI(){
                // LAPIC registers are 32-bit aligned; cast via uintptr_t to avoid alignment warning
                while (*(volatile U32*)((uintptr_t)ACPI::LAPIC::g_LapicVirtualBase + LAPIC_REG_ICR_LOW) & (1 << 12)) {
                    // Tunggu bit "Delivery Status" (12) menjadi 0
                    asm volatile("pause");
                }
            }

            VOID SendInit(U32 ApicID){
                LAPIC::LapicWrite(LAPIC_REG_ICR_HIGH, (ApicID << 24));
                LAPIC::LapicWrite(LAPIC_REG_ICR_LOW, 0x00004500); // INIT, level-triggered, assert
                WaitForIPI();
            }

            VOID SendSIPI(U32 ApicID, U8 Vector){
                LAPIC::LapicWrite(LAPIC_REG_ICR_HIGH, (ApicID << 24));
                LAPIC::LapicWrite(LAPIC_REG_ICR_LOW, 0x00004600 | Vector); // SIPI, level-triggered, assert
                WaitForIPI();
            }

            VOID InitSMP(){
                using namespace Printk;
                if (g_CpuCount <= 1) {
                    Write(Level::LOG_INFO, "SMP: only one CPU detected, skipping AP startup\n");
                    return;
                }

                g_ApReadyTarget = g_CpuCount > 0 ? (g_CpuCount - 1) : 0;
                g_ApReadyCount = 0;
                g_ApStackCount = 0;

                // Ensure trampoline area is reserved and copy the embedded binary there.
                PageAlloc::PhysicalReserveLow(TRAMPOLINE_PHYS & ~(PAGE_SIZE - 1), 1);

                const U8* tramp_bin_start = _binary_build_firmware_acpi_madt_smpmod_rmpmlmtramp_bin_start;
                const U8* tramp_bin_end   = _binary_build_firmware_acpi_madt_smpmod_rmpmlmtramp_bin_end;

                SIZE_T tramp_size = (SIZE_T)(tramp_bin_end - tramp_bin_start);
                if (tramp_size > PAGE_SIZE) {
                    Write(Level::LOG_ERR, "SMP: trampoline binary size %zu exceeds one page\n", tramp_size);
                    return;
                }

                SIZE_T blob_size = tramp_size;
                if (blob_size < sizeof(TrampolineMetadata)) {
                    Write(Level::LOG_ERR, "SMP: trampoline blob too small for metadata (%zu)\n", blob_size);
                    return;
                }

                const TrampolineMetadata* meta = reinterpret_cast<const TrampolineMetadata*>(
                    tramp_bin_start + (blob_size - sizeof(TrampolineMetadata)));

                if (meta->signature != TRAMP_META_SIGNATURE) {
                    Write(Level::LOG_ERR, "SMP: trampoline metadata signature mismatch (0x%08x)\n",
                          (unsigned)meta->signature);
                    return;
                }
                if (meta->image_size > blob_size) {
                    Write(Level::LOG_ERR, "SMP: metadata image size (%u) exceeds blob size (%zu)\n",
                          (unsigned)meta->image_size, blob_size);
                    return;
                }

                phys_memcpy(TRAMPOLINE_PHYS, tramp_bin_start, tramp_size);
                // Zero pad remaining bytes to avoid stale data
                phys_memset(TRAMPOLINE_PHYS + tramp_size, 0, PAGE_SIZE - tramp_size);

                const SIZE_T pml4_off    = (SIZE_T)meta->pml4_offset;
                const SIZE_T gdtptr_off  = (SIZE_T)meta->gdtptr_offset;
                const SIZE_T stack_off   = (SIZE_T)meta->ap_stack_offset;
                const SIZE_T entry_off   = (SIZE_T)meta->ap_main_offset;
                const SIZE_T apic_off    = (SIZE_T)meta->apic_offset;

                if (meta->version != 1) {
                    Write(Level::LOG_ERR, "SMP: unsupported trampoline metadata version %u\n", (unsigned)meta->version);
                    return;
                }

                UPTR trampoline_pml4_phys = EnsureTrampolinePML4();
                if (!trampoline_pml4_phys) {
                    Write(Level::LOG_ERR, "SMP: failed to allocate trampoline page tables\n");
                    return;
                }

                // Patch data fields
                phys_write_u32(TRAMPOLINE_PHYS, pml4_off, (U32)trampoline_pml4_phys);
                phys_write_u64(TRAMPOLINE_PHYS, gdtptr_off, 0); // Optional: real kernel GDT pointer if needed
                // Allocate stack per AP (2 pages) simplistic
                for (U32 i = 1; i < g_CpuCount; ++i) {
                    UPTR stack_phys = PageAlloc::PhysicalAllocPages(2); // 8K stack
                    if (!stack_phys) {
                        Write(Level::LOG_ERR, "SMP: failed alloc stack for APIC %u\n", (unsigned)g_CpuApicIds[i]);
                        continue;
                    }
                    UPTR stack_top_virt = stack_phys + 2 * PAGE_SIZE + HHDM_BASE;
                    phys_write_u64(TRAMPOLINE_PHYS, stack_off, stack_top_virt); // use HHDM-mapped stack
                    phys_write_u64(TRAMPOLINE_PHYS, entry_off, (U64)(UPTR)ap_main_entry);
                    phys_write_u64(TRAMPOLINE_PHYS, apic_off, (U64)g_CpuApicIds[i]);

                    if (g_ApStackCount < MAX_CPU_COUNT) {
                        g_ApStacks[g_ApStackCount++] = {
                            g_CpuApicIds[i],
                            stack_phys,
                            stack_top_virt
                        };
                    }

                    Write(Level::LOG_INFO, "SMP: sending INIT+SIPI to APIC %u (stack phys 0x%p)\n", (unsigned)g_CpuApicIds[i], (void*)stack_phys);
                    SendInit(g_CpuApicIds[i]);
                    Arch::Time::Sleep(1); // ~1ms delay between INIT and SIPI
                    SendSIPI(g_CpuApicIds[i], TRAMPOLINE_VECTOR);
                    Arch::Time::Sleep(1); // second SIPI spacing
                }

                if (g_ApStackCount) {
                    Write(Level::LOG_DEBUG, "SMP: AP stack audit\n");
                    for (U32 idx = 0; idx < g_ApStackCount; ++idx) {
                        const ApStackRecord& rec = g_ApStacks[idx];
                        Write(Level::LOG_DEBUG,
                              "  APIC %u -> stack phys 0x%p, top virt 0x%p\n",
                              (unsigned)rec.apic_id,
                              (void*)rec.stack_phys,
                              (void*)rec.stack_top_virt);
                    }
                }

                WaitForApBarrier();
            }
        }
    }
}

VOID ACPI::LAPIC::SMP::SignalApReady(U32 apic_id) {
    asm volatile ("" ::: "memory");
    const U32 new_count = AtomicFetchAdd(&g_ApReadyCount, 1) + 1;
    Printk::Write(Printk::Level::LOG_DEBUG,
                  "SMP: AP[%u] signalled ready (%u/%u)\n",
                  (unsigned)apic_id,
                  new_count,
                  g_ApReadyTarget);
}