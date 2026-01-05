#pragma once
#include <rosval.h>

namespace AC97 {
    // Register Offset untuk NABMBAR (Bus Master)
    enum NABM_Regs {
        PCM_IN_BDBAR  = 0x00, // Buffer Descriptor List Base Address
        PCM_IN_LVI    = 0x05, // Last Valid Index
        PCM_IN_SR     = 0x06, // Status Register
        PCM_OUT_BDBAR = 0x10, // Playback Buffer Descriptor Base
        PCM_OUT_LVI   = 0x15, // Playback Last Valid Index
        PCM_OUT_SR    = 0x16, // Playback Status Register
        PCM_OUT_CR    = 0x1B  // Control Register
    };

    // Struktur Buffer Descriptor (Wajib 8-byte aligned)
    struct AC97_BufferDescriptor {
        U32 Pointer;      // Alamat fisik buffer suara
        U16 Length;       // Jumlah sample (dalam frame)
        U16 Flags;        // Bit 15: Interrupt on completion, Bit 14: BUP
    } __attribute__((packed));

    NORESULTFUNC Initialize(U8 bus, U8 device, U8 func);
    NORESULTFUNC PlayTestSound(U32 hz);
}