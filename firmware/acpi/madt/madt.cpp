#define PRINTK_MODULE_NAME "MADT"
#include "madt.hpp"
#include <rossys.hpp>
#include <mm.hpp>
#include <logging.hpp>

namespace ACPI {
    VOID ParseMADT(){
        if(g_MADT == nullptr){
            Printk::Write(Printk::Level::LOG_ERR, " MADT is null, cannot parse\n");
            return;
        }

        const MadtHeader *MADT = (const MadtHeader *)g_MADT;

        g_LocalApicAddress = MADT->LocalApicAddress;
        Printk::Write(Printk::Level::LOG_INFO, " MADT Local APIC Address: %p\n", (void*)(uintptr_t)g_LocalApicAddress);

        const uint8_t *base = (const uint8_t*)MADT;
        const uint8_t *end = base + MADT->Header.Length;
        const uint8_t *cur = base + sizeof(MadtHeader);

        while (cur < end) {
            const MadtEntryHeader *Entry = (const MadtEntryHeader *)cur;

            if (Entry->Length == 0) {
                Printk::Write(Printk::Level::LOG_ERR, " MADT entry with zero length, aborting parse\n");
                break;
            }

            switch (Entry->Type) {
                case 0: {
                    const MadtEntryLocalApic *LAPIC = (const MadtEntryLocalApic *)cur;
                    if (g_CpuCount < MAX_CPU_COUNT) {
                        g_CpuApicIds[g_CpuCount] = LAPIC->ApicId;
                        g_CpuCount++;
                        Printk::Write(Printk::Level::LOG_INFO, " MADT Local APIC Entry: ProcessorID=%u ApicID=%u Flags=0x%08x\n",
                            (unsigned)LAPIC->ProcessorId, (unsigned)LAPIC->ApicId, (unsigned)LAPIC->Flags);
                    }
                    break;
                }

                case 1: {
                    const MadtEntryIoApic *IOAPIC = (const MadtEntryIoApic *)cur;
                    g_IoApicAddress = IOAPIC->IoApicAddress;
                    Printk::Write(Printk::Level::LOG_INFO, " MADT I/O APIC Entry: IoApicID=%u Address=%p GSIBase=%u\n",
                        (unsigned)IOAPIC->IoApicId,
                        (void*)(uintptr_t)IOAPIC->IoApicAddress,
                        (unsigned)IOAPIC->GlobalSystemInterruptBase);
                    break;
                }

                default: {
                    // Unknown/unsupported entry — skip
                    break;
                }
            }

            cur += Entry->Length;
        }

        Printk::Write(Printk::Level::LOG_INFO, " MADT parsing complete: CPU Count=%u I/O APIC Address=%p\n",
            (unsigned)g_CpuCount, (void*)(uintptr_t)g_IoApicAddress);
    }

    namespace LAPIC{
        volatile U8* g_LapicVirtualBase = nullptr;

        VOID LapicWrite(uint32_t reg, uint32_t value) {
            // LAPIC base virtual address sudah di-map ke memori fisik APIC (MMIO)
            volatile uint32_t* lapic = reinterpret_cast<volatile uint32_t*>(g_LapicVirtualBase);
            lapic[reg / 4] = value;
        }

        U32 LapicRead(uint32_t reg) {
            volatile uint32_t* lapic = reinterpret_cast<volatile uint32_t*>(g_LapicVirtualBase);
            return lapic[reg / 4];
        }

        VOID InitializeLAPIC(){
            if(g_LocalApicAddress == 0) return;

            constexpr U64 APIC_MSR_ENABLE_BIT = (1ULL << 11);
            constexpr U64 APIC_MSR_BASE_MASK = 0x00000000FFFFF000ULL;

            U64 apicMsr = Arch::MSR::Read(Arch::MSR::IA32_APIC_BASE);
            U64 msrPhysBase = apicMsr & APIC_MSR_BASE_MASK;
            U64 desiredPhysBase = (U64)(g_LocalApicAddress & ~(PAGE_SIZE - 1));

            if (msrPhysBase != desiredPhysBase) {
                apicMsr &= ~APIC_MSR_BASE_MASK;
                apicMsr |= desiredPhysBase;
                Printk::Write(Printk::Level::LOG_INFO, " Adjusting IA32_APIC_BASE from phys 0x%p to 0x%p\n",
                    (void*)(UPTR)msrPhysBase, (void*)(UPTR)desiredPhysBase);
            }

            if ((apicMsr & APIC_MSR_ENABLE_BIT) == 0) {
                apicMsr |= APIC_MSR_ENABLE_BIT;
                Printk::Write(Printk::Level::LOG_INFO, " Enabling Local APIC via IA32_APIC_BASE MSR\n");
            }

            Arch::MSR::Write(Arch::MSR::IA32_APIC_BASE, apicMsr);

            UPTR LapicPhysPage = g_LocalApicAddress & ~(PAGE_SIZE - 1);
            g_LapicVirtualBase = (volatile U8*)PageAlloc::VirtualAllocPages(1);
            if(!g_LapicVirtualBase){
                Printk::Write(Printk::Level::LOG_ERR, " Failed to allocate virtual page for LAPIC\n");
                return;
            }

            PFLAGS Flags = PAGE_PRESENT | PAGE_RW | PAGE_PCD;
            if(!PageAlloc::MapPages(KernelPML4, LapicPhysPage, (UPTR)g_LapicVirtualBase, 1, Flags)) {
                Printk::Write(Printk::Level::LOG_ERR, " Failed to map LAPIC registers\n");
                g_LapicVirtualBase = nullptr;
                return;
            }

            // Ensure task priority is clear so the LAPIC will accept all priorities
            LapicWrite(0x080, 0);

            U32 SivVal = LapicRead(LAPIC_REG_SIVR);
            SivVal |= APIC_SOFTWARE_ENABLE;
            SivVal = (SivVal & 0xFFFFFF00) | 0xFF; // Set spurious vector to 0xFF
            LapicWrite(LAPIC_REG_SIVR, SivVal);

            Printk::Write(Printk::Level::LOG_INFO, " LAPIC initialized at phys 0x%p (virt %p), SIVR=0x%08x\n",
                (void*)(UPTR)g_LocalApicAddress, (void*)g_LapicVirtualBase, (unsigned)SivVal);
        }
    }

    namespace IOAPIC{
        volatile U8* g_IoApicVirtualBase = nullptr;

        VOID IOAPICWrite(uint8_t reg, uint32_t val) {
            volatile uint32_t* ioapic = reinterpret_cast<volatile uint32_t*>(g_IoApicVirtualBase);
            ioapic[0] = reg;  // IOREGSEL (offset 0x00)
            ioapic[4] = val;  // IOWIN (offset 0x10)
        }

        U32 IOAPICRead(uint8_t reg) {
            volatile uint32_t* ioapic = reinterpret_cast<volatile uint32_t*>(g_IoApicVirtualBase);
            ioapic[0] = reg; // select
            return ioapic[4]; // read data
        }

        VOID IOApicRedirect(U8 GSI, U8 Vector, IOAPICFLAGS Flags){
            U32 Low32 = (U32)Vector | Flags;
            U32 High32 = (0 << 24);

            U8 RegOffset = REDTBL_ENTRY_FOR_GSI(GSI);
            IOAPIC::IOAPICWrite(RegOffset, Low32);
            IOAPIC::IOAPICWrite(RegOffset + 1, High32);
        }

        VOID IOApicRedirectToCPU(U8 GSI, U8 Vector, IOAPICFLAGS Flags, U8 destApicId){
            U32 Low32 = (U32)Vector | Flags;
            U32 High32 = ((U32)destApicId << 24);

            U8 RegOffset = REDTBL_ENTRY_FOR_GSI(GSI);
            IOAPIC::IOAPICWrite(RegOffset, Low32);
            IOAPIC::IOAPICWrite(RegOffset + 1, High32);

            Printk::Write(Printk::Level::LOG_INFO, " IOAPIC: redirected GSI %u -> vector 0x%02x dest APIC %u\n",
                (unsigned)GSI, (unsigned)Vector, (unsigned)destApicId);
        }

        VOID InitializeIOAPIC(){
            if(g_IoApicAddress == 0){
                Printk::Write(Printk::Level::LOG_ERR, " IOAPIC address is zero, cannot initialize\n");
                return;
            }

            UPTR IoApicPhysPage = g_IoApicAddress & ~(PAGE_SIZE - 1);
            g_IoApicVirtualBase = (volatile U8*)PageAlloc::VirtualAllocPages(1);
            if(!g_IoApicVirtualBase){
                Printk::Write(Printk::Level::LOG_ERR, " Failed to allocate virtual page for IOAPIC\n");
                return;
            }

            PFLAGS Flags = PAGE_PRESENT | PAGE_RW | PAGE_PCD;
            if(!PageAlloc::MapPages(KernelPML4, IoApicPhysPage, (UPTR)g_IoApicVirtualBase, 1, Flags)) {
                Printk::Write(Printk::Level::LOG_ERR, " Failed to map IOAPIC registers\n");
                g_IoApicVirtualBase = nullptr;
                return;
            }

            Printk::Write(Printk::Level::LOG_INFO, " IOAPIC initialized at phys 0x%p (virt %p)\n",
                (void*)(UPTR)g_IoApicAddress, (void*)g_IoApicVirtualBase);

            // Contoh: Redirect GSI 1 (Keyboard, IRQ 1) ke vector 0x21
            // Flags: 0x0000 = Active High, Edge Triggered
            IOApicRedirect(1, 0x21, IOAPIC_FLAGS_DEFAULT); 
            
            // Contoh: Redirect GSI 12 (PS/2 Mouse, IRQ 12) ke vector 0x2C
            IOApicRedirect(12, 0x2C, IOAPIC_FLAGS_DEFAULT);

            // Redirect COM1 (serial) GSI 4 -> vector 0x24 so serial works
            // after legacy PIC is masked. Many systems route COM1 at GSI4.
            IOApicRedirect(4, 0x24, IOAPIC_FLAGS_DEFAULT);

            // Ensure PIT (GSI 0) is redirected through the IOAPIC to the
            // legacy vector 0x20 so PIT ticks continue after the legacy
            // PIC is disabled later by the kernel. Without this redirection
            // PIT::ticks will stop once the PIC is disabled and any code
            // that depends on PIT ticks (Arch::Time::Sleep) will hang.
            IOApicRedirect(0, 0x20, IOAPIC_FLAGS_DEFAULT); // GSI0 -> IRQ0 (vector 0x20)

            // Leave the cascade (GSI 2) mapping alone — the PIC disable
            // path in the kernel will mask the 8259 appropriately.
        }
    }
}
