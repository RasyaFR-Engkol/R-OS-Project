#define PRINTK_MODULE_NAME "Userland"
#include <rosval.h>
#include <rossys.hpp>
#include <logging.hpp>

#define GDT_KERNEL_CODE_SELECTOR 0x08 // Entry GDT ke-1 (Ring 0 Code)
#define GDT_KERNEL_DATA_SELECTOR 0x10 // Entry GDT ke-2 (Ring 0 Data)
#define GDT_USER_DATA_SELECTOR   0x1B // Entry GDT ke-3 (Ring 3 Data, 0x18 | 3)
#define GDT_USER_CODE_SELECTOR   0x23 // Entry GDT ke-4 (Ring 3 Code, 0x20 | 3)

ABI_C VOID Syscall_AsmEntry();

namespace Userland{
    VOID Syscall_Init(){
        using Arch::MSR::IA32_EFER;
        using Arch::MSR::IA32_LSTAR;
        using Arch::MSR::IA32_STAR;
        using Arch::MSR::IA32_FMASK;

        U64 Efer = Arch::MSR::Read(IA32_EFER);
        Efer |= 0x1; // Set bit 0 (LME)
        Arch::MSR::Write(IA32_EFER, Efer);

        Arch::MSR::Write(IA32_LSTAR, (U64)&Syscall_AsmEntry);

        U64 Star = 0;
        Star |= (U64)GDT_KERNEL_CODE_SELECTOR << 32;

        Star |= (U64)(GDT_USER_DATA_SELECTOR - 8) << 48;

        Arch::MSR::Write(IA32_STAR, Star);

        Arch::MSR::Write(IA32_FMASK, 0x200);

        //Printk::Write(Printk::LOG_INFO, "Userland initialized. EFER=0x%llx LSTAR=0x%llx STAR=0x%llx FMASK=0x%llx\n",
        //    Arch::MSR::Read(IA32_EFER),
        //    Arch::MSR::Read(IA32_LSTAR),
        //    Arch::MSR::Read(IA32_STAR),
        //    Arch::MSR::Read(IA32_FMASK)
        //);
    }
}