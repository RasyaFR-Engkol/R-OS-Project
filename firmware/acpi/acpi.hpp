#pragma once

#include <rosval.h>

namespace ACPI{
    struct RsdpDescriptor {
        char     Signature[8];
        uint8_t  Checksum;
        char     OEMID[6];
        uint8_t  Revision;
        uint32_t RsdtAddress; // Alamat fisik dari RSDT (ACPI 1.0)
 
        // --- Mulai dari sini, ini hanya ada di ACPI 2.0+ ---
        // Jika Revision == 0, 14 byte di bawah ini tidak ada
        uint32_t Length;
        uint64_t XsdtAddress; // Alamat fisik dari XSDT (ACPI 2.0+)
        uint8_t  ExtendedChecksum;
        uint8_t  Reserved[3];
    } __attribute__((packed));

    struct SDTHeader {
        char     Signature[4];
        uint32_t Length;
        uint8_t  Revision;
        uint8_t  Checksum;
        char     OEMID[6];
        char     OEMTableID[8];
        uint32_t OEMRevision;
        uint32_t CreatorID;
        uint32_t CreatorRevision;
    } __attribute__((packed));

    // Header untuk tabel MADT
    struct MadtHeader {
        SDTHeader Header;
        uint32_t LocalApicAddress; // Alamat FISIK dari Local APIC (LAPIC)
        uint32_t Flags;
    } __attribute__((packed));

    // Setiap entri di dalam MADT dimulai dengan ini
    struct MadtEntryHeader {
        uint8_t Type;
        uint8_t Length;
    } __attribute__((packed));

    // Entri Tipe 0: Processor Local APIC (Ini adalah CPU core!)
    struct MadtEntryLocalApic {
        MadtEntryHeader Header;
        uint8_t ProcessorId; // ID dari ACPI (bisa jadi tidak urut)
        uint8_t ApicId;      // ID dari APIC (ini yang kita pakai)
        uint32_t Flags;      // 1 = Enabled
    } __attribute__((packed));

    // Entri Tipe 1: I/O APIC (Untuk interrupt dari hardware)
    struct MadtEntryIoApic {
        MadtEntryHeader Header;
        uint8_t IoApicId;
        uint8_t Reserved;
        uint32_t IoApicAddress; // Alamat FISIK dari I/O APIC
        uint32_t GlobalSystemInterruptBase;
    } __attribute__((packed));

    // Generic Address Structure (GAS)
    // Ini adalah format "aneh" yang dipakai ACPI untuk mendeskripsikan
    // alamat register (bisa di I/O port, memory, dll)
    struct GenericAddressStructure {
        uint8_t AddressSpace;    // 0 = System Memory, 1 = System I/O
        uint8_t BitWidth;
        uint8_t BitOffset;
        uint8_t AccessSize;
        uint64_t Address; // Alamat fisik memori atau nomor port I/O
    } __attribute__((packed));


    // Header untuk tabel FADT
    // (Ini adalah versi sederhana, FADT aslinya SANGAT BESAR)
    struct FadtHeader {
        SDTHeader Header;
        uint32_t FirmwareCtrl;   // Alamat fisik 'FACS'
        uint32_t Dsdt;           // Alamat fisik 'DSDT'
        uint8_t  Reserved;
        uint8_t  PreferredPmProfile;
        uint16_t SciInterrupt;
        uint32_t SmiCmd;         // Port I/O untuk SMI Command (penting!)
        uint8_t  AcpiEnable;     // Nilai yang harus ditulis ke SmiCmd
        uint8_t  AcpiDisable;    // Nilai untuk disable
        uint8_t  S4BiosReq;
        uint8_t  PstateControl;
        uint32_t Pm1aEventBlock;
        uint32_t Pm1bEventBlock;
        uint32_t Pm1aControlBlock; // Port I/O untuk power mgmt (shutdown!)
        uint32_t Pm1bControlBlock;
        // ... (masih banyak field lain, tapi kita fokus di sini dulu) ...

        // Versi ACPI 1.0 berhenti di sini
        
        // Versi ACPI 2.0+ punya alamat 64-bit
        // yang menggantikan alamat 32-bit di atas
        GenericAddressStructure X_FirmwareCtrl;
        GenericAddressStructure X_Dsdt;
        GenericAddressStructure X_Pm1aEventBlock;
        GenericAddressStructure X_Pm1bEventBlock;
        GenericAddressStructure X_Pm1aControlBlock;
        GenericAddressStructure X_Pm1bControlBlock;
        // ... (dan masih banyak lagi) ...

        // Register untuk REBOOT
        GenericAddressStructure ResetReg;
        uint8_t  ResetValue;

    } __attribute__((packed));

    #define MAX_CPU_COUNT 16

    VOID Initialize();
    VOID ParseROOTSDT();
    VOID ParseMADT();
    VOID ParseFADT();

    /* Globals exposed for modular parsers (defined in initialize.cpp) */
    extern const void* g_FADT;
    extern const void* g_MADT;
    extern U8 g_CpuApicIds[];
    extern U32 g_CpuCount;
    extern U32 g_IoApicAddress;
    extern U32 g_LocalApicAddress;
    extern GenericAddressStructure g_ResetRegister;
    extern U8 g_ResetValue;
    extern U32 g_SmiCommandPort;
    extern U8 g_AcpiEnableValue;
    extern U32 g_Pm1aControlPort;
    extern U32 g_Pm1bControlPort;
}