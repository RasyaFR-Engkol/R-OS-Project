#include "../acpica/source/include/acpi.h"
#include "acpi.hpp"
#include <bootinfo.h>
#include <mm.hpp>

extern "C" {

ACPI_STATUS AcpiInitializeSubsystem(void) { return AE_OK; }
ACPI_STATUS AcpiInitializeTables(void* A, int B, int C) { (void)A; (void)B; (void)C; return AE_OK; }
ACPI_STATUS AcpiLoadTables(void) { return AE_OK; }
ACPI_STATUS AcpiEnableSubsystem(unsigned long F) { (void)F; return AE_OK; }
ACPI_STATUS AcpiInitializeObjects(unsigned long F) { (void)F; return AE_OK; }

ACPI_STATUS AcpiGetTable(const char* Signature, unsigned Index, ACPI_TABLE_HEADER** Out) {
    if (!Signature || !Out) return AE_BAD_PARAMETER;
    const BootInfo* bi = BootInfoGet();
    if (!bi || !bi->has_acpi || !bi->acpi.rsdp) return AE_NOT_FOUND;
    const ACPI::RsdpDescriptor* rsdp = nullptr;
    UPTR ptr = (UPTR)bi->acpi.rsdp;
    if (ptr >= (UPTR)HHDM_BASE) rsdp = (const ACPI::RsdpDescriptor*)ptr; else rsdp = (const ACPI::RsdpDescriptor*)HHDM_PhysToVirt(ptr);
    if (!rsdp) return AE_NOT_FOUND;

    unsigned found = 0;
    if (bi->acpi.is_xsdp) {
        const ACPI::SDTHeader* xsdt = (const ACPI::SDTHeader*)HHDM_PhysToVirt((UPTR)rsdp->XsdtAddress);
        if (!xsdt) return AE_NOT_FOUND;
        SIZE_T entries = (xsdt->Length - sizeof(ACPI::SDTHeader)) / 8;
        const uint8_t* base = (const uint8_t*)xsdt + sizeof(ACPI::SDTHeader);
        for (SIZE_T i = 0; i < entries; ++i) {
            uint64_t v = 0;
            const uint8_t* p8 = base + i * 8;
            for (int b = 0; b < 8; ++b) v |= (uint64_t)p8[b] << (8 * b);
            const ACPI::SDTHeader* h = (const ACPI::SDTHeader*)HHDM_PhysToVirt((UPTR)v);
            if (!h) continue;
            if (h->Signature[0] == Signature[0] && h->Signature[1] == Signature[1] && h->Signature[2] == Signature[2] && h->Signature[3] == Signature[3]) {
                if (found == Index) { *Out = const_cast<ACPI_TABLE_HEADER*>(reinterpret_cast<const ACPI_TABLE_HEADER*>(h)); return AE_OK; }
                found++;
            }
        }
    } else {
        const ACPI::SDTHeader* rsdt = (const ACPI::SDTHeader*)HHDM_PhysToVirt((UPTR)rsdp->RsdtAddress);
        if (!rsdt) return AE_NOT_FOUND;
        SIZE_T entries = (rsdt->Length - sizeof(ACPI::SDTHeader)) / 4;
        const uint8_t* base = (const uint8_t*)rsdt + sizeof(ACPI::SDTHeader);
        for (SIZE_T i = 0; i < entries; ++i) {
            uint32_t v = 0;
            const uint8_t* p4 = base + i * 4;
            for (int b = 0; b < 4; ++b) v |= (uint32_t)p4[b] << (8 * b);
            const ACPI::SDTHeader* h = (const ACPI::SDTHeader*)HHDM_PhysToVirt((UPTR)v);
            if (!h) continue;
            if (h->Signature[0] == Signature[0] && h->Signature[1] == Signature[1] && h->Signature[2] == Signature[2] && h->Signature[3] == Signature[3]) {
                if (found == Index) { *Out = const_cast<ACPI_TABLE_HEADER*>(reinterpret_cast<const ACPI_TABLE_HEADER*>(h)); return AE_OK; }
                found++;
            }
        }
    }
    return AE_NOT_FOUND;
}

} // extern C
