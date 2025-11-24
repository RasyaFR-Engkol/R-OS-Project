#define PRINTK_MODULE_NAME "IDTINIT"
#include <rosval.h>
#include "rossys.hpp"
#include "idt.hpp"
#include "serial.hpp"
#include "string.hpp"
#include <logging.hpp>

static IDTEntry G_IDT[256];
static InterruptHandler G_Handlers[256]; // per-vector registered handlers

// Vector Bitmap for Interrupt Allocation
static U8 G_VectorBitmap[256 / 8]; // 32 bytes

inline void BmpSet(U8 vec)   { G_VectorBitmap[vec >> 3] |=  (U8)(1u << (vec & 7)); }
inline void BmpClear(U8 vec) { G_VectorBitmap[vec >> 3] &= (U8)~(1u << (vec & 7)); }
inline bool BmpTest(U8 vec)  { return (G_VectorBitmap[vec >> 3] >> (vec & 7)) & 1u; }

ABI_C void IsrStub_PageFault();
extern "C" void* IrqStub_Table[]; // exported from irqstub.asm

namespace IDT {
    VOID MCEHandler(void* context) {
        Arch::ASM::Cli(); // Disable interrupts immediately
        // Read some useful MSRs to help debugging the Machine Check Exception.
        U64 efer = Arch::MSR::ReadEFER();
        U64 apic_base = Arch::MSR::Read(Arch::MSR::IA32_APIC_BASE);

        // Log MSR values at error level to aid post-mortem analysis.
        Printk::Write(Printk::Level::LOG_ERR,
                      "IDT: MCE detected. MSR values: EFER=0x%016llx APIC_BASE=0x%016llx\n",
                      efer, apic_base);

        // Read Machine-Check Global Capability and Status MSRs to determine
        // how many MC banks exist and the global MCG status.
        const U32 IA32_MCG_CAP = 0x179u;
        const U32 IA32_MCG_STATUS = 0x17Au;
        U64 mcg_cap = Arch::MSR::Read(IA32_MCG_CAP);
        U64 mcg_status = Arch::MSR::Read(IA32_MCG_STATUS);
        U32 bank_count = (U32)(mcg_cap & 0xFFu);
        if (bank_count == 0) bank_count = 0; // explicit for clarity
        if (bank_count > 64) bank_count = 64; // sanity cap

        Printk::Write(Printk::Level::LOG_ERR,
                      "IDT: IA32_MCG_CAP=0x%016llx banks=%u IA32_MCG_STATUS=0x%016llx\n",
                      mcg_cap, bank_count, mcg_status);

        // Iterate each bank and print STATUS/ADDR/MISC when non-zero.
        for (U32 bank = 0; bank < bank_count; ++bank) {
            U32 base = 0x400u + (bank * 4u);
            U64 status = Arch::MSR::Read(base + 1u); // IA32_MCi_STATUS
            if (status != 0) {
                U64 addr = Arch::MSR::Read(base + 2u);  // IA32_MCi_ADDR
                U64 misc = Arch::MSR::Read(base + 3u);  // IA32_MCi_MISC
                Printk::Write(Printk::Level::LOG_ERR,
                              "IDT: MCE bank %u: STATUS=0x%016llx ADDR=0x%016llx MISC=0x%016llx\n",
                              (unsigned)bank, status, addr, misc);
            }
        }

        // Panic-level message and halt the system.
        Printk::Write(Printk::Level::LOG_EMERG, "IDT: Machine Check Exception occurred! System halt.\n");
        while (1) { asm volatile("hlt"); }
    }

    VOID InitializeIDT() {
        String::Memset(G_VectorBitmap, 0, sizeof(G_VectorBitmap));
        String::Memset(G_Handlers, 0, sizeof(G_Handlers));
        
        U16 KernelCS = 0x08;
        U8 TrapFlags = IDT_FLAG_PRESENT | IDT_FLAG_DPL0 | IDT_FLAG_GATE_TRAP;
        U8 IntFlags  = IDT_FLAG_PRESENT | IDT_FLAG_DPL0 | IDT_FLAG_GATE_INTERRUPT;

        // Exceptions
        G_IDT[14].Set((VOID*)IsrStub_PageFault, KernelCS, TrapFlags);
        G_IDT[0x12].Set((VOID*)MCEHandler, KernelCS, TrapFlags); // SIMD FP Exception

        // Hardware IRQs and MSI range: map 0..207 to vectors 0x20..0x20+207 (up to 0xEF)
        for (unsigned i = 0; i < 208; ++i) {
            void* stub = ((void**)IrqStub_Table)[i];
            G_IDT[0x20 + i].Set(stub, KernelCS, IntFlags);
        }

        struct __attribute__((packed)) {
            U16 limit;
            UPTR base;
        } IDTR;
        IDTR.limit = sizeof(G_IDT) - 1;
        IDTR.base = (UPTR)&G_IDT;
        asm volatile("lidt %0" : : "m"(IDTR));
        Serial::Printf("Done IDT\n");
    }

    // Correctly spelled API
    void RegisterInterruptHandler(U8 vector, InterruptHandler GHandler) {
        // vector is U8 (0..255) so the range check is unnecessary
        G_Handlers[vector] = GHandler;
        Printk::Write(Printk::Level::LOG_DEBUG,
                      "IDT: Registered handler for vector 0x%02X at %p\n",
                      vector, (void*)GHandler.handler);
    }

    // Back-compat for earlier misspelling
    void RegsiterInterruptHandler(U8 vector, InterruptHandler GHandler) {
        RegisterInterruptHandler(vector, GHandler);
    }

    // Invoke if present
    void InvokeInterruptHandler(U8 vector, void *context) {
        // vector is U8 (0..255) so the range check is unnecessary
        auto fn = G_Handlers[vector].handler;
        if (fn) fn(context);
    }

    U8 AllocateVector(){
        for(VAL32 V = 0x84; V < 0xF0; V++){
            if (!BmpTest(V)) {
                BmpSet(V);
                return (U8)V;
            }
        }
        Printk::Write(Printk::Level::LOG_ERR, "IDT: No free interrupt vectors available for allocation!\n");
        return 0;
    }

    VOID FreeVector(U8 vector){
        if (vector >= 0x84) { // Jangan free vektor hardware
            BmpClear(vector);
            G_Handlers[vector].handler = nullptr;
        }
    }
}