#define PRINTK_MODULE_NAME "ACPI_HPET"
#include <rosval.h>
#include <rossys.hpp>
#include <logging.hpp>
#include <mm.hpp>
#include "../acpi.hpp"

namespace ACPI{
    VOID ParseHPET(){
        if(g_HPET == nullptr){
            Printk::Write(Printk::Level::LOG_ERR, " HPET is null, cannot parse\n");
            return;
        }

        const HpetHeader *HPET = (const HpetHeader*)g_HPET;

        if(HPET->BaseAddress.AddressSpace != 0){
            Printk::Write(Printk::Level::LOG_ERR, " HPET base address is not system memory, cannot parse\n");
            return;
        }

        g_HpetBaseAddress = HPET->BaseAddress.Address;

        Printk::Write(Printk::Level::LOG_INFO, " HPET Base Address: %p\n", (void*)(uintptr_t)g_HpetBaseAddress);

        UPTR HpetPhysPage = g_HpetBaseAddress & ~(PAGE_SIZE - 1);
        volatile U8* HpetVirtualBase = (volatile U8*)PageAlloc::VirtualAllocPages(1);
        if(!HpetVirtualBase){
            Printk::Write(Printk::Level::LOG_ERR, " Failed to allocate virtual page for HPET\n");
            return;
        }

        if(!PageAlloc::MapPages(KernelPML4, HpetPhysPage, (UPTR)HpetVirtualBase, 1, PAGE_PRESENT | PAGE_RW | PAGE_PCD)) {
            Printk::Write(Printk::Level::LOG_ERR, " Failed to map HPET registers\n");
            return;
        }

        Printk::Write(Printk::Level::LOG_INFO, " HPET initialized at phys 0x%p (virt %p)\n",
            (void*)(UPTR)g_HpetBaseAddress, (void*)HpetVirtualBase);

        g_HpetVirtAddress = HpetVirtualBase;
    }
}