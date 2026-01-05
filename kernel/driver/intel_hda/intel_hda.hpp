#pragma once
#include <rosval.h>
#include <rossys.hpp> // Untuk tipe data dan Arch::Time

namespace IntelHDA {

    // --- Register Offsets (Memory Mapped) ---
    #define HDA_REG_GCAP      0x00  // Global Capabilities
    #define HDA_REG_VMIN      0x02  // Minor Version
    #define HDA_REG_VMAJ      0x03  // Major Version
    #define HDA_REG_OUTPAY    0x04  // Output Payload Capability
    #define HDA_REG_INPAY     0x06  // Input Payload Capability
    #define HDA_REG_GCTL      0x08  // Global Control (PENTING BUAT RESET)
    #define HDA_REG_WAKEEN    0x0C  // Wake Enable
    #define HDA_REG_STATESTS  0x0E  // State Status (Buat deteksi Codec)
    #define HDA_REG_GSTS      0x10  // Global Status
    #define HDA_REG_INTCTL    0x20  // Interrupt Control
    #define HDA_REG_INTSTS    0x24  // Interrupt Status

    // --- CORB & RIRB Offsets (Kita pakai di Step 2) ---
    #define HDA_REG_CORBLBASE 0x40
    #define HDA_REG_CORBUBASE 0x44
    #define HDA_REG_CORBWP    0x48
    #define HDA_REG_CORBRP    0x4A
    #define HDA_REG_CORBCTL   0x4C
    #define HDA_REG_CORBSTS   0x4D
    #define HDA_REG_CORBSIZE  0x4E

    #define HDA_REG_RIRBLBASE 0x50
    #define HDA_REG_RIRBUBASE 0x54
    #define HDA_REG_RIRBWP    0x58
    #define HDA_REG_RIRBINTCNT 0x5A
    #define HDA_REG_RIRBCTL   0x5C
    #define HDA_REG_RIRBSTS   0x5D
    #define HDA_REG_RIRBSIZE  0x5E

    // Class Controller
    class HDAController {
    private:
        U8  Bus, Device, Function;
        U64 MMIOBasePhys;
        U64 MMIOBaseVirt; // Alamat Virtual setelah mapping

        // Virtual Address untuk kita tulis/baca
        VOLATILE U32* CORBBuffer; 
        VOLATILE U64* RIRBBuffer; 
        
        // Jumlah Entry
        VOLATILE U16 CORBEntries;
        VOLATILE U16 RIRBEntries;

        // Software Pointer (Kita yang pegang)
        VOLATILE U16 CORBWritePtr; 
        VOLATILE U16 RIRBReadPtr;

        // Helper Read/Write MMIO
        // Menggunakan volatile agar compiler tidak mengoptimasi akses memori
        inline U32 Read32(U32 offset) {
            return *(volatile U32*)(MMIOBaseVirt + offset);
        }
        inline void Write32(U32 offset, U32 val) {
            *(volatile U32*)(MMIOBaseVirt + offset) = val;
        }
        // HDA banyak main di register 16-bit dan 8-bit juga
        inline U16 Read16(U32 offset) {
            return *(volatile U16*)(MMIOBaseVirt + offset);
        }
        inline void Write16(U32 offset, U16 val) {
            *(volatile U16*)(MMIOBaseVirt + offset) = val;
        }
        inline U8 Read8(U32 offset) {
            return *(volatile U8*)(MMIOBaseVirt + offset);
        }
        inline void Write8(U32 offset, U8 val) {
            *(volatile U8*)(MMIOBaseVirt + offset) = val;
        }

        // Fungsi internal
        BOOLFUNC ResetController();
        BOOLFUNC InitCORB();
        BOOLFUNC InitRIRB();
        BOOL WaitForBit(U32 reg, U32 mask, U32 val, U32 timeoutMs);

        enum WidgetType {
            WIDGET_AUDIO_OUTPUT = 0,
            WIDGET_AUDIO_INPUT = 1,
            WIDGET_MIXER = 2,
            WIDGET_SELECTOR = 3,
            WIDGET_PIN_COMPLEX = 4,
            WIDGET_POWER = 5,
            WIDGET_VOLUME_KNOB = 6,
            WIDGET_BEEP_GEN = 7,
            WIDGET_VENDOR = 0xF
        };

    public:
        VOIDFUNC Initialize(U8 bus, U8 dev, U8 func);
        U32 SendVerb(U8 cad, U8 nid, U32 payload);
        VOIDFUNC ScanCodec();
        NORESULTFUNC ParseWidgets(BYTE CAD);
    };

    extern HDAController GlobalController;
}