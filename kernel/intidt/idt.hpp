#pragma once

#include <rosval.h>

struct __attribute__((packed)) IDTEntry{
    U16 isr_low;        // 16 bit pertama dari alamat ISR (0-15)
    U16 segment_selector; // Segment selector GDT (biasanya 0x08)
    U8  ist;            // Interrupt Stack Table (biarkan 0 dulu)
    U8  attributes;     // Tipe & Atribut Gate (P, DPL, Type)
    U16 isr_mid;        // 16 bit tengah dari alamat ISR (16-31)
    U32 isr_high;       // 32 bit atas dari alamat ISR (32-63)
    U32 reserved;       // Harus 0

    // Fungsi helper untuk mengatur entry ini dengan mudah
    void Set(void* isr, U16 gdt_segment, U8 flags, U8 ist_index = 0) {
        UPTR addr = (UPTR)isr;
        
        this->isr_low    = (U16)(addr & 0xFFFF);
        this->isr_mid    = (U16)((addr >> 16) & 0xFFFF);
        this->isr_high   = (U32)((addr >> 32) & 0xFFFFFFFF);
        
        this->segment_selector = gdt_segment;
        this->attributes = flags;
        this->ist        = ist_index;
        this->reserved   = 0;
    }
};

// FLAGS
// --- Atribut Flags untuk IdtEntry ---

// P: Entry ini "Present" atau valid
#define IDT_FLAG_PRESENT (1 << 7)

// DPL: Descriptor Privilege Level (0 = Kernel, 3 = User)
#define IDT_FLAG_DPL0 (0 << 5)
#define IDT_FLAG_DPL3 (3 << 5)

// Tipe Gate:
// Interrupt Gate (0xE): Menonaktifkan interrupt (cli) saat dipanggil.
//                      Bagus untuk hardware IRQ.
#define IDT_FLAG_GATE_INTERRUPT 0xE

// Trap Gate (0xF): TIDAK menonaktifkan interrupt saat dipanggil.
//                  Bagus untuk exceptions (Page Fault) dan syscalls.
#define IDT_FLAG_GATE_TRAP 0xF

// Simple handler wrapper
typedef struct{
    void (*handler)(void *context);
} InterruptHandler;

namespace IDT{
    VOID InitializeIDT();
    // Register and invoke interrupt handlers dynamically
    void RegisterInterruptHandler(U8 vector, InterruptHandler handler);
    // Convenience overload to register with raw function pointer
    inline void RegisterInterruptHandler(U8 vector, void (*fn)(void *context)) {
        RegisterInterruptHandler(vector, InterruptHandler{fn});
    }
    // Called by dispatchers to run a registered handler, if any
    void InvokeInterruptHandler(U8 vector, void *context = nullptr);

    U8 AllocateVector();
    VOID FreeVector(U8 vector);
}