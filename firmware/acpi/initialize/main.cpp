#define PRINTK_MODULE_NAME "ACPIMain"
#include <rosval.h>
#include <rossys.hpp>
#include <framebuffer.hpp>
#include <bootinfo.h>
#include <serial.hpp>
#include <logging.hpp>
#include <mm.hpp>
#include <string.hpp>
#include "../acpi.hpp"
#include "../fadt/fadt.hpp"
#include "../madt/madt.hpp"
#include "../driver/timer/timer.hpp"
#include "../bgrt/bgrt.hpp"

// TODO: Buatkan initialize ACPI di sini
// Kita inging membuat initialize ACPI, mencari XSDT, lalu cari tabel 
// ACPI lain. kita bisa pake disini untuk inisialisasi perangkat-perangkat
// yang membutuhkan ACPI.
// 
// Urutan Inisialisasi ACPI:
// 1. Cari RSDP (Root System Description Pointer) di memori.
// 2. Dari RSDP, dapatkan alamat XSDT (Extended System Description Table).
// 3. Baca XSDT untuk mendapatkan daftar tabel ACPI lainnya.
// 4. Inisialisasi perangkat-perangkat berdasarkan tabel ACPI yang ditemukan.

extern "C" {
    #include "../../acpica/source/include/acpi.h"
    #include "../../acpica/source/include/acpixf.h"
}

namespace ACPI{
    using namespace String;
    // Legacy RootSDT scanning is deprecated; ACPICA will load tables.
    static const void* g_RootSDT = nullptr; // retained for logging only
    static bool g_IsXSDT = FALSE;           // retained for logging only
    const void* g_FADT = nullptr;
    const void* g_MADT = nullptr;
    const void* g_HPET = nullptr;
    #define MAX_CPU_COUNT 16
    U8 g_CpuApicIds[MAX_CPU_COUNT];
    U32 g_CpuCount = 0;
    U32 g_IoApicAddress = 0;
    U32 g_LocalApicAddress = 0;
    U64 g_HpetBaseAddress = 0;
    volatile U8* g_HpetVirtAddress = nullptr;
    GenericAddressStructure g_ResetRegister;
    U8 g_ResetValue = 0;
    U32 g_SmiCommandPort = 0;
    U8 g_AcpiEnableValue = 0;
    U32 g_Pm1aControlPort = 0;
    U32 g_Pm1bControlPort = 0;

    static BOOL ValidateChecksum(const U8* table, U32 length){
        U8 sum = 0;
        for(U32 i = 0; i < length; ++i){
            sum += table[i];
        }
        return (sum == 0);
    }

    // Initialize ACPICA core; RSDP physical pointer is obtained via AcpiOsGetRootPointer.
    // Parameter removed (was unused) for cleanliness.
    static void InitializeACPICA(){
        // Basic ACPICA early init
        if (ACPI_FAILURE(AcpiInitializeSubsystem())) {
            Printk::Write(Printk::Level::LOG_ERR, " ACPICA: InitializeSubsystem failed\n");
            return;
        }
        // Install all tables from RSDP (physical addresses)
        if (ACPI_FAILURE(AcpiInitializeTables(NULL, 16, FALSE))) {
            Printk::Write(Printk::Level::LOG_ERR, " ACPICA: InitializeTables failed\n");
            return;
        }
        if (ACPI_FAILURE(AcpiLoadTables())) {
            Printk::Write(Printk::Level::LOG_ERR, " ACPICA: LoadTables failed\n");
            return;
        }
        if (ACPI_FAILURE(AcpiEnableSubsystem(ACPI_NO_HANDLER_INIT | ACPI_NO_EVENT_INIT))) {
            Printk::Write(Printk::Level::LOG_ERR, " ACPICA: EnableSubsystem failed\n");
            return;
        }
        if (ACPI_FAILURE(AcpiInitializeObjects(ACPI_NO_HANDLER_INIT | ACPI_NO_EVENT_INIT))) {
            Printk::Write(Printk::Level::LOG_ERR, " ACPICA: InitializeObjects failed\n");
            return;
        }
        Printk::Write(Printk::Level::LOG_INFO, " ACPICA initialized (RSDT/XSDT loaded)\n");

        // Minimal runtime sanity: fetch FADT via ACPICA and log a few fields
        ACPI_TABLE_FADT *fadt = nullptr;
    // Cast required: ACPICA's ACPI_STRING is (char*), macro expands to string literal (const char*)
    ACPI_STATUS st = AcpiGetTable((ACPI_STRING)ACPI_SIG_FADT, 0, (ACPI_TABLE_HEADER **)&fadt);
        if (ACPI_SUCCESS(st) && fadt) {
            Printk::Write(Printk::Level::LOG_INFO, " FADT rev=%u, SMI_CMD=0x%08x, ACPI_EN=0x%02x, PM1a=0x%08x\n",
                (unsigned)fadt->Header.Revision,
                (unsigned)fadt->SmiCommand, (unsigned)fadt->AcpiEnable,
                (unsigned)fadt->Pm1aControlBlock);
        } else {
            Printk::Write(Printk::Level::LOG_WARNING, " ACPICA: FADT not available via AcpiGetTable (status=0x%x)\n", (unsigned)st);
        }
    }

    VOID Initialize(){
        const BootInfo* bi = BootInfoGet();

        if(!bi){
            Printk::Write(Printk::Level::LOG_ERR, " BootInfo not found, cannot initialize ACPI\n");
            return;
        }

        // Cek jika system ini punya informasi ACPI. kalo enggak, return
        if(!bi->has_acpi){
            Printk::Write(Printk::Level::LOG_ERR, " No ACPI information in BootInfo\n");
            return;
        }

        Printk::Write(Printk::Level::LOG_INFO, " ACPI Initializer by ROS Project\n Copyright (C) 2024 Rasya Team\n");
        // Kita gabisa dereference langsung. wajib pake MM untuk mapping HHDM

        /* bi->acpi.rsdp may contain a physical address. Do not dereference
         * it directly. Map it into the higher-half direct mapping (HHDM)
         * before accessing fields. If the pointer already points into the
         * HHDM (high addresses), use it directly. */
        UPTR rsdp_ptr = (UPTR)bi->acpi.rsdp;
        const RsdpDescriptor *rsdp = nullptr;
        if (rsdp_ptr == 0) {
            Printk::Write(Printk::Level::LOG_ERR, " RSDP pointer is NULL\n");
            return;
        }
        if (rsdp_ptr >= (UPTR)HHDM_BASE) {
            rsdp = (const RsdpDescriptor *)rsdp_ptr;
        } else {
            rsdp = (const RsdpDescriptor *)HHDM_PhysToVirt(rsdp_ptr);
        }
        const U8 *RSDPBytes = (const U8*)rsdp;

        /* Correct RSDP signature is "RSD PTR " (8 bytes including spaces). */
        if (Strncmp(rsdp->Signature, "RSD PTR ", 8) != 0) {
            Printk::Write(Printk::Level::LOG_ERR, " Invalid RSDP signature\n");
            return;
        }

        // Validasi Checksum terlebih dahulu
        BOOL ChecksumVallid = FALSE;
        if(bi->acpi.is_xsdp){
            // Ini punya ACPI 2,0+
            ChecksumVallid = ValidateChecksum(RSDPBytes, rsdp->Length);
        } else{
            // Punya ACPI 1.0
            ChecksumVallid = ValidateChecksum(RSDPBytes, 20); // Panjang RSDP ACPI 1.0 adalah 20 byte
        }

        if(!ChecksumVallid){
            Printk::Write(Printk::Level::LOG_ERR, " Invalid RSDP checksum\n");
            return;
        }

        if (bi->acpi.is_xsdp) {
            Printk::Write(Printk::Level::LOG_INFO, " ACPI 2.0+ detected, using XSDT at %p\n",
                (void*)(uintptr_t)rsdp->XsdtAddress);
            g_RootSDT = (const void*)(uintptr_t)rsdp->XsdtAddress;
            g_IsXSDT = TRUE;
        } else {
            Printk::Write(Printk::Level::LOG_INFO, " ACPI 1.0 detected, using RSDT at %p\n",
                (void*)(uintptr_t)rsdp->RsdtAddress);
            g_RootSDT = (const void*)(uintptr_t)rsdp->RsdtAddress;
            g_IsXSDT = FALSE;
        }

    /* Initialize ACPICA core and let it load tables. */
    InitializeACPICA();

    /* Prefer ACPICA to fetch core tables; fall back to legacy parser only if needed */
    unsigned found = 0;
    {
        ACPI_TABLE_HEADER *hdr = nullptr;
        if (ACPI_SUCCESS(AcpiGetTable((ACPI_STRING)ACPI_SIG_FADT, 0, &hdr)) && hdr) {
            g_FADT = (const void*)hdr;
            Printk::Write(Printk::Level::LOG_INFO, " Using ACPICA FADT @ %p\n", g_FADT);
            ParseFADT();
            found++;
            // Optional: enable only if not already enabled; legacy Enable() checks bit
            Enable();
        }
    }
    {
        // MADT signature historically is "APIC"
        ACPI_TABLE_HEADER *hdr = nullptr;
        if (ACPI_SUCCESS(AcpiGetTable((ACPI_STRING)"APIC", 0, &hdr)) && hdr) {
            g_MADT = (const void*)hdr;
            Printk::Write(Printk::Level::LOG_INFO, " Using ACPICA MADT/APIC @ %p\n", g_MADT);
            ParseMADT();
            found++;
        }
    }
    {
        ACPI_TABLE_HEADER *hdr = nullptr;
        if (ACPI_SUCCESS(AcpiGetTable((ACPI_STRING)"HPET", 0, &hdr)) && hdr) {
            g_HPET = (const void*)hdr;
            Printk::Write(Printk::Level::LOG_INFO, " Using ACPICA HPET @ %p\n", g_HPET);
            ParseHPET();
            found++;
        }
    }

    // Optional tables: MCFG (PCIe ECAM), WAET, BGRT — parse/log if present
    {
        ACPI_TABLE_MCFG *mcfg = nullptr;
        if (ACPI_SUCCESS(AcpiGetTable((ACPI_STRING)ACPI_SIG_MCFG, 0, (ACPI_TABLE_HEADER **)&mcfg)) && mcfg) {
            // Compute number of allocation entries
            U32 count = 0;
            if (mcfg->Header.Length > sizeof(ACPI_TABLE_MCFG)) {
                count = (UINT32)((mcfg->Header.Length - sizeof(ACPI_TABLE_MCFG)) / sizeof(ACPI_MCFG_ALLOCATION));
            }
            Printk::Write(Printk::Level::LOG_INFO, " Using ACPICA MCFG @ %p, %u segment allocation(s)\n", mcfg, (unsigned)count);
            ACPI_MCFG_ALLOCATION *alloc = (ACPI_MCFG_ALLOCATION *)((UINT8*)mcfg + sizeof(ACPI_TABLE_MCFG));
            for (U32 i = 0; i < count; ++i) {
                const ACPI_MCFG_ALLOCATION &a = alloc[i];
                Printk::Write(Printk::Level::LOG_INFO,
                    "  MCFG[%u]: seg=%u bus=[%u..%u] ECAM=0x%016lx\n",
                    (unsigned)i,
                    (unsigned)a.PciSegment,
                    (unsigned)a.StartBusNumber,
                    (unsigned)a.EndBusNumber,
                    (unsigned long)a.Address);
            }
        } else {
            // Not all platforms provide MCFG; this is optional.
        }
    }
    {
        ACPI_TABLE_WAET *waet = nullptr;
        if (ACPI_SUCCESS(AcpiGetTable((ACPI_STRING)ACPI_SIG_WAET, 0, (ACPI_TABLE_HEADER **)&waet)) && waet) {
            Printk::Write(Printk::Level::LOG_INFO, " Using ACPICA WAET @ %p Flags=0x%08x\n",
                waet, (unsigned)waet->Flags);
        }
    }
    {
        ACPI_TABLE_BGRT *bgrt = nullptr;
        if (ACPI_SUCCESS(AcpiGetTable((ACPI_STRING)ACPI_SIG_BGRT, 0, (ACPI_TABLE_HEADER **)&bgrt)) && bgrt) {
            Printk::Write(Printk::Level::LOG_INFO,
                " Using ACPICA BGRT @ %p ver=%u status=0x%02x type=%u addr=%p off=(%u,%u)\n",
                bgrt,
                (unsigned)bgrt->Version,
                (unsigned)bgrt->Status,
                (unsigned)bgrt->ImageType,
                (void*)(uintptr_t)bgrt->ImageAddress,
                (unsigned)bgrt->ImageOffsetX,
                (unsigned)bgrt->ImageOffsetY);
            // Attempt to draw the firmware boot logo via BGRT if available.
            ACPI::BGRT::ShowLogoOnce();
        }
    }
    if (found == 0) {
        // Fallback: legacy walk (should be rare)
        ParseROOTSDT();
    }

        // After parsing ACPI tables, initialize interrupt controllers if MADT
        // provided Local APIC / I/O APIC addresses. These call into
        // firmware/acpi/madt implementations.
        if (g_LocalApicAddress != 0) {
            LAPIC::InitializeLAPIC();
            // Do NOT start the LAPIC timer here: timer calibration requires
            // PIT interrupts to be active. Timer will be started by the
            // kernel initialization sequence after PIT is running and
            // interrupts are enabled.
        }
        if (g_IoApicAddress != 0) {
            IOAPIC::InitializeIOAPIC();
        }

        // Defer SMP startup until after PIT/LAPIC timer and interrupts are
        // fully enabled in the main kernel initialization path.
    }

    VOID ParseROOTSDT(){
        // Legacy path: retained as fallback when AcpiGetTable fails
        if(g_RootSDT == nullptr){
            Printk::Write(Printk::Level::LOG_ERR, " Root SDT is null, cannot parse\n");
            return;
        }

        const SDTHeader* RootHeader = (const SDTHeader*)HHDM_PhysToVirt((UPTR)g_RootSDT);

        if(!ValidateChecksum((const U8*)RootHeader, RootHeader->Length)){
            Printk::Write(Printk::Level::LOG_ERR, " Invalid Root SDT checksum\n");
            return;
        }

        U32 EntryCount = 0;
        UPTR EntryArray = (UPTR)(RootHeader + 1);
        U32 ArrayLength = RootHeader->Length - sizeof(SDTHeader);

        if(g_IsXSDT){
            EntryCount = ArrayLength / 8; // 64-bit entries
        } else{
            EntryCount = ArrayLength / 4; // 32-bit entries
        }

        /* Print signature safely: Printk implementation may not support the
         * precision form "%.4s", so print four characters individually to
         * avoid format parsing issues that corrupt subsequent arguments. */
        Printk::Write(Printk::Level::LOG_INFO, " Parsing Root SDT: Signature=%c%c%c%c Length=%u Entries=%u\n",
            RootHeader->Signature[0], RootHeader->Signature[1],
            RootHeader->Signature[2], RootHeader->Signature[3],
            (unsigned)RootHeader->Length, (unsigned)EntryCount);

        /* Sanity-check length to avoid runaway counts from corrupted headers. */
        if (RootHeader->Length < sizeof(SDTHeader) || RootHeader->Length > 0x100000) {
            Printk::Write(Printk::Level::LOG_ERR, " Root SDT length %u suspicious, aborting\n", (unsigned)RootHeader->Length);
            return;
        }

        // Iterate through entries
        for(U32 i = 0; i < EntryCount; ++i){
            UPTR TablePhysADDR = 0;

            if(g_IsXSDT){
                // 64-bit entry
                UPTR* entry_ptr = (UPTR*)(EntryArray + i * 8);
                TablePhysADDR = *entry_ptr;
            } else{
                // 32-bit entry
                U32* entry_ptr = (U32*)(EntryArray + i * 4);
                TablePhysADDR = (UPTR)(*entry_ptr);
            }

            if(TablePhysADDR == 0){
                Printk::Write(Printk::Level::LOG_WARNING, " Entry %u has NULL address, skipping\n", (unsigned)i);
                continue;
            }

            const SDTHeader* TableHeader = (const SDTHeader*)HHDM_PhysToVirt(TablePhysADDR);

            if(!ValidateChecksum((const U8*)TableHeader, TableHeader->Length)){
                Printk::Write(Printk::Level::LOG_ERR, " Invalid checksum for table %c%c%c%c at %p\n",
                    TableHeader->Signature[0], TableHeader->Signature[1],
                    TableHeader->Signature[2], TableHeader->Signature[3],
                    (void*)(uintptr_t)TablePhysADDR);
                continue;
            }

            /* Debug: print each table signature and its physical address */
            Printk::Write(Printk::Level::LOG_INFO, " Table: %c%c%c%c @ %p len=%u\n",
                TableHeader->Signature[0], TableHeader->Signature[1],
                TableHeader->Signature[2], TableHeader->Signature[3],
                (void*)(uintptr_t)TablePhysADDR, (unsigned)TableHeader->Length);

            // Check signature and store pointer. Accept common historical
            // variants: "FACP" is the legacy name for FADT, and "APIC"
            // is the signature used for the MADT on many firmwares.
            const char *sig = TableHeader->Signature;
            if (Strncmp(sig, "FADT", 4) == 0 || Strncmp(sig, "FACP", 4) == 0) {
                g_FADT = TableHeader;
                Printk::Write(Printk::Level::LOG_INFO, " -> Found FADT/FACP at virt %p\n", g_FADT);
                ParseFADT();
                Enable();
            } else if (Strncmp(sig, "MADT", 4) == 0 || Strncmp(sig, "APIC", 4) == 0) {
                // MADT is commonly published with signature "APIC" by some firmwares
                g_MADT = TableHeader;
                Printk::Write(Printk::Level::LOG_INFO, " -> Found MADT/APIC at virt %p\n", g_MADT);
                ParseMADT();
            } else if (Strncmp(sig, "HPET", 4) == 0) {
                g_HPET = TableHeader;
                Printk::Write(Printk::Level::LOG_INFO, " -> Found HPET\n");
                ParseHPET();
            } else if (Strncmp(TableHeader->Signature, "MCFG", 4) == 0) {
                // ...
                Printk::Write(Printk::Level::LOG_INFO, " -> Found MCFG (PCIe)\n");
            }
        }
    }

    VOID Enable(){
        if(g_Pm1aControlPort == 0 || g_SmiCommandPort == 0){
            Printk::Write(Printk::Level::LOG_ERR, " Cannot enable ACPI: PM1aControlPort or SmiCommandPort is zero\n");
            return;
        }

        if(Port::Inw(g_Pm1aControlPort) & 1){
            Printk::Write(Printk::Level::LOG_INFO, " ACPI is already enabled\n");
            return;
        }

        // Tambah PRINTK LOG PORT dan VALUE
        Printk::Write(Printk::Level::LOG_INFO, " Enabling ACPI: SMI_CMD=0x%08x ACPI_EN=0x%02x PM1a_CTRL=0x%08x\n",
            (unsigned)g_SmiCommandPort, (unsigned)g_AcpiEnableValue, (unsigned)g_Pm1aControlPort);

        Port::Outb(g_SmiCommandPort, g_AcpiEnableValue);

        VAL32 TimeoutMS = 1000;
        BOOL Success = FALSE;
        while(TimeoutMS > 0){
            if(Port::Inw(g_Pm1aControlPort) & 1){
                Success = TRUE;
                break;
            }
            Arch::Time::Sleep(1);
            TimeoutMS--;
        }

        if(Success){
            Printk::Write(Printk::Level::LOG_INFO, " ACPI enabled successfully\n");
        } else{
            Printk::Write(Printk::Level::LOG_ERR, " ACPI enable timed out after 1000ms\n");
        }
    }

    // MADT and FADT parsing have been moved to modular implementations
    // in firmware/acpi/madt and firmware/acpi/fadt respectively.

}

