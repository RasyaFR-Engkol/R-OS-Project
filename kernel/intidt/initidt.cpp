#define PRINTK_MODULE_NAME "IDTINIT"
#include <rosval.h>
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
    VOID InitializeIDT() {
        String::Memset(G_VectorBitmap, 0, sizeof(G_VectorBitmap));
        String::Memset(G_Handlers, 0, sizeof(G_Handlers));
        
        U16 KernelCS = 0x08;
        U8 TrapFlags = IDT_FLAG_PRESENT | IDT_FLAG_DPL0 | IDT_FLAG_GATE_TRAP;
        U8 IntFlags  = IDT_FLAG_PRESENT | IDT_FLAG_DPL0 | IDT_FLAG_GATE_INTERRUPT;

        // Exceptions
        G_IDT[14].Set((VOID*)IsrStub_PageFault, KernelCS, TrapFlags);

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
    }

    // Back-compat for earlier misspelling
    void RegsiterInterruptHandler(U8 vector, InterruptHandler GHandler) {
        RegisterInterruptHandler(vector, GHandler);
    }

    // Invoke if present
    void InvokeInterruptHandler(U8 vector) {
        // vector is U8 (0..255) so the range check is unnecessary
        auto fn = G_Handlers[vector].handler;
        if (fn) fn();
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