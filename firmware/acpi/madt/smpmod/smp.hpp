#pragma once
#include <rosval.h>

#define LAPIC_REG_ICR_LOW  0x300
#define LAPIC_REG_ICR_HIGH 0x310

namespace ACPI{
    namespace LAPIC{
        namespace SMP{
            struct TrampolineData{
                struct{
                    U16 Limit;
                    U16 Base;
                } __attribute__((packed)) GdtPointer;

                U32 Pml4Addr;
                U64 GDTKernelPointer;
                U64 ApStackAddr;
                U64 ApMainFunc;
                U64 ApicId;
            } __attribute__((packed));

            // Initialize SMP: copy trampoline to 0x8000, patch data, send INIT+SIPI
            VOID InitSMP();
            // AP entry (called by trampoline) signature
            typedef void (*ApMainFunc)(U32 apic_id);

            // Called by an AP once it has switched to the kernel environment.
            VOID SignalApReady(U32 apic_id);
        }
    }
}