#define PRINTK_MODULE_NAME "FADT"
#include "fadt.hpp"
#include "port.hpp"
#include <mm.hpp>
#include <logging.hpp>
#include <string.hpp>

namespace ACPI {
    VOID ParseFADT(){
        if(g_FADT == nullptr){
            Printk::Write(Printk::Level::LOG_ERR, " FADT is null, cannot parse\n");
            return;
        }

        const FadtHeader *FADT = (const FadtHeader *)g_FADT;

        // Read some useful fields (be conservative: check lengths)
        Printk::Write(Printk::Level::LOG_INFO, " FADT length=%u\n", (unsigned)FADT->Header.Length);

        // Copy commonly used legacy 32-bit fields directly (valid across revs)
        g_SmiCommandPort = FADT->SmiCmd;
        g_AcpiEnableValue = FADT->AcpiEnable;
        g_Pm1aControlPort = FADT->Pm1aControlBlock;
        g_Pm1bControlPort = FADT->Pm1bControlBlock;

        // Robustly fetch ResetReg/ResetValue using spec offsets to avoid
        // structure layout mismatches across FADT revisions.
        const U8* base = (const U8*)FADT;
        if (FADT->Header.Length >= 129) { // ensure ResetReg (12B) + ResetValue (1B) present
            // ACPI 2.0+: ResetReg at offset 116 (0x74), ResetValue at 128 (0x80)
            const SIZE_T offResetReg = 116;
            const SIZE_T offResetVal = 128;
            if (FADT->Header.Length >= offResetVal + 1) {
                const GenericAddressStructure* gas = (const GenericAddressStructure*)(base + offResetReg);
                g_ResetRegister = *gas;
                g_ResetValue = *(const U8*)(base + offResetVal);
            }
        } else {
            // No ResetReg available (ACPI 1.0), set to zero to disable ACPI reset
            String::Memset(&g_ResetRegister, 0, sizeof(g_ResetRegister));
            g_ResetValue = 0;
        }

        Printk::Write(Printk::Level::LOG_INFO, " FADT: SMI_CMD=0x%08x ACPI_EN=0x%02x PM1a_CTRL=0x%08x PM1b_CTRL=0x%08x ResetReg {space=%u width=%u off=%u size=%u addr=0x%llx} ResetVal=0x%02x\n",
            (unsigned)g_SmiCommandPort, (unsigned)g_AcpiEnableValue, (unsigned)g_Pm1aControlPort, (unsigned)g_Pm1bControlPort,
            (unsigned)g_ResetRegister.AddressSpace, (unsigned)g_ResetRegister.BitWidth,
            (unsigned)g_ResetRegister.BitOffset, (unsigned)g_ResetRegister.AccessSize,
            (unsigned long long)g_ResetRegister.Address, (unsigned)g_ResetValue);
    }
}

namespace Arch{
    namespace Power{
        
        // Robust AML _S5_ parser: extract SLP_TYPa/TYPb from the DSDT.
        // Scans for the literal "_S5_" and then searches forward for a
        // PackageOp (0x12). Properly decodes the AML PackageLength field
        // (1..4 bytes) and bounds-checks all reads to avoid overruns.
        static BOOL ParseSx(U8* dsdt, U32 len, const U8 sig[4], U8* outA, U8* outB) {
            if (!dsdt || len < 8) return FALSE;

            auto parse_pkg_length = [&](U32 offset, U32 available, U32 &consumed, U32 &pkglen)->bool{
                if (available == 0) return false;
                U8 lead = dsdt[offset];
                U32 byte_count = (lead >> 4) & 0x07; // 0..3
                pkglen = (lead & 0x0F);
                consumed = 1;
                if (byte_count > 3) return false; // spec allows up to 3 extra
                if (available < 1 + byte_count) return false;
                for (U32 i = 0; i < byte_count; ++i) {
                    pkglen |= ((U32)dsdt[offset + 1 + i]) << (4 + 8 * i);
                    consumed++;
                }
                return true;
            };

            for (U32 i = 0; i + 4 <= len; ++i) {
                if (dsdt[i] == sig[0] && dsdt[i+1] == sig[1] && dsdt[i+2] == sig[2] && dsdt[i+3] == sig[3]) {
                    // Found the name string; search forward a short distance for PackageOp
                    U32 p = i + 4;
                    U32 search_limit = (p + 32 < len) ? (p + 32) : len;
                    bool found = false;
                    for (; p < search_limit; ++p) {
                        if (dsdt[p] == 0x12) { found = true; break; }
                    }
                    if (!found) continue;

                    // p points at PackageOp
                    ++p;
                    if (p >= len) continue;

                    // Decode pkgLength properly
                    U32 consumed = 0, pkglen = 0;
                    if (!parse_pkg_length(p, len - p, consumed, pkglen)) continue;
                    p += consumed;
                    if (p >= len) continue;

                    // After pkgLength, the next byte is typically the element count
                    U8 elemCount = dsdt[p++];
                    if (elemCount < 2) continue;
                    // Sanity: ensure package payload fits
                    if ((U32)(p - (i + 4) /*relative*/ + pkglen) > len - (i + 4)) {
                        // package overruns buffer, skip
                        continue;
                    }

                    // Safely read AML data objects for the first two elements
                    auto readObj = [&](U32 &offset, U8 &val)->bool{
                        if (offset >= len) return false;
                        U8 op = dsdt[offset++];
                        switch (op) {
                            case 0x0A: { // ByteConst
                                if (offset >= len) return false; 
                                val = dsdt[offset++]; 
                                return true;
                            }
                            case 0x0B: { // WordConst
                                if (offset + 1 >= len) return false; 
                                // Kode Anda sudah benar: mengambil LSB (U8) dari WordConst
                                val = (U8)dsdt[offset]; 
                                offset += 2; 
                                return true;
                            }
                            case 0x0C: { // DWordConst
                                if (offset + 3 >= len) return false; 
                                // Kode Anda sudah benar: mengambil LSB (U8) dari DWordConst
                                val = (U8)dsdt[offset]; 
                                offset += 4; 
                                return true;
                            }
                            default:
                                return false;
                        }
                    };

                    U32 off = p; U8 a = 0, b = 0, tmp = 0;
                    if (!readObj(off, a)) continue;
                    if (!readObj(off, b)) continue;
                    // Consume rest of elements loosely to advance parser safely
                    for (U8 e = 2; e < elemCount; ++e) { if (!readObj(off, tmp)) break; }
                    *outA = a; *outB = b; return TRUE;
                }
            }
            return FALSE;
        }

        static BOOL ParseS3(U8* dsdt, U32 len, U8* outA, U8* outB) {
            const U8 sig[4] = {'_', 'S', '3', '_'};
            return ParseSx(dsdt, len, sig, outA, outB);
        }

        // FUNGSI LAMA (ParseS5), sekarang memanggil ParseSx
        static BOOL ParseS5(U8* dsdt, U32 len, U8* outA, U8* outB) {
            const U8 sig[4] = {'_', 'S', '5', '_'};
            return ParseSx(dsdt, len, sig, outA, outB);
        }

        VOID Reboot(){
            Printk::Write(Printk::Level::LOG_INFO, "[Power] Initiating system reboot via ACPI FADT ResetReg\n");
                if (ACPI::g_ResetRegister.Address == 0) {
                    Printk::Write(Printk::Level::LOG_WARNING, "[Power] Cannot reboot: ACPI ResetReg address is zero\n");
                    return;
                }

                /* ACPI Generic Address Structure: AddressSpace 0 = system memory,
                 * 1 = I/O port. Handle both, but validate memory addresses to avoid
                 * mapping obviously bogus physical addresses (which causes a page
                 * fault). If the physical address looks suspicious, fall back to
                 * attempting an I/O port write using the low 16 bits. */
                switch (ACPI::g_ResetRegister.AddressSpace) {
                    case 0: {
                        UPTR phys = (UPTR)ACPI::g_ResetRegister.Address;
                        if (phys == 0) {
                            Printk::Write(Printk::Level::LOG_WARNING, "[Power] ResetReg physical address is zero, aborting\n");
                            return;
                        }

                        /* Map the MMIO region into kernel virtual memory before
                         * writing. HHDM_PhysToVirt only covers RAM regions that are
                         * already identity-mapped; MMIO ranges (like PCI config or
                         * chipset registers) need explicit page table mappings. */
                        UPTR phys_page_base = phys & PAGE_ADDR_MASK;
                        UPTR offset = phys - phys_page_base;
                        SIZE_T map_bytes = (SIZE_T)offset + 1; /* we only need 1 byte */
                        SIZE_T pages = (map_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

                        void *vaddr = PageAlloc::VirtualAllocPages(pages);
                        if (!vaddr) {
                            Printk::Write(Printk::Level::LOG_WARNING, "[Power] VirtualAllocPages failed for MMIO mapping, falling back to I/O write\n");
                            Port::Outb((U16)phys, ACPI::g_ResetValue);
                            return;
                        }

                        if (!PageAlloc::MapPages(KernelPML4, phys_page_base, (UPTR)vaddr, pages, PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT)) {
                            Printk::Write(Printk::Level::LOG_WARNING, "[Power] MapPages failed for phys %p, freeing vmem and falling back to I/O\n", (void*)(uintptr_t)phys_page_base);
                            PageAlloc::VirtualFreePages(vaddr, pages);
                            Port::Outb((U16)phys, ACPI::g_ResetValue);
                            return;
                        }

                        volatile U8* ResetRegVirt = (volatile U8*)((UPTR)vaddr + offset);
                        Printk::Write(Printk::Level::LOG_INFO, "[Power] Writing 0x%02x to MMIO phys %p (virt %p) for reboot\n",
                            (unsigned)ACPI::g_ResetValue, (void*)(uintptr_t)phys, (void*)ResetRegVirt);
                        *ResetRegVirt = ACPI::g_ResetValue;

                        // We intentionally do not unmap the pages here; system will reboot.
                        break;
                    }

                    case 1: {
                        UPTR port = (UPTR)ACPI::g_ResetRegister.Address;
                        Printk::Write(Printk::Level::LOG_INFO, "[Power] Writing 0x%02x to I/O port 0x%llx for reboot\n",
                            (unsigned)ACPI::g_ResetValue, (unsigned long long)port);
                        Port::Outb((U16)port, ACPI::g_ResetValue);
                        break;
                    }

                    default: {
                        Printk::Write(Printk::Level::LOG_ERR, "[Power] Unknown AddressSpace %u in ACPI ResetReg, cannot reboot\n",
                            (unsigned)ACPI::g_ResetRegister.AddressSpace);
                        break;
                    }
                }
        }

        VOID Shutdown(){
            // Try ACPI S5 via PM1x control using _S5_ values.
            U8 s5a=0, s5b=0; BOOL haveS5 = FALSE;
            U8 s3a =0, s3b =0; [[maybe_unused]]BOOL haveS3 = FALSE;
            do {
                // Map DSDT header to get its length, then map full table
                // Use 32-bit Dsdt field from parsed FADT (sufficient on QEMU)
                const ACPI::FadtHeader* FADT = (const ACPI::FadtHeader*)ACPI::g_FADT;
                if (!FADT) break;
                UPTR dsdt_phys = (UPTR)FADT->Dsdt;
                if (!dsdt_phys) break;
                // Map first page to read SDTHeader
                void* hdr_v = PageAlloc::VirtualAllocPages(1);
                if (!hdr_v) break;
                if (!PageAlloc::MapPages(KernelPML4, dsdt_phys & PAGE_ADDR_MASK, (UPTR)hdr_v, 1, PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT)) {
                    PageAlloc::VirtualFreePages(hdr_v, 1); break;
                }
                const ACPI::SDTHeader* H = (const ACPI::SDTHeader*)((UPTR)hdr_v + (dsdt_phys & (PAGE_SIZE-1)));
                U32 dsdt_len = H->Length;
                // Map entire DSDT length
                PageAlloc::VirtualFreePages(hdr_v, 1);
                UPTR base_phys = dsdt_phys & PAGE_ADDR_MASK;
                UPTR off = dsdt_phys - base_phys;
                SIZE_T pages = (off + dsdt_len + PAGE_SIZE - 1) / PAGE_SIZE;
                void* v = PageAlloc::VirtualAllocPages(pages);
                if (!v) break;
                if (!PageAlloc::MapPages(KernelPML4, base_phys, (UPTR)v, pages, PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT)) {
                    PageAlloc::VirtualFreePages(v, pages); break;
                }
                U8* dsdt = (U8*)((UPTR)v + off);
                haveS5 = ParseS5(dsdt + sizeof(ACPI::SDTHeader), dsdt_len - sizeof(ACPI::SDTHeader), &s5a, &s5b);
                if(ParseS3(dsdt + sizeof(ACPI::SDTHeader), dsdt_len - sizeof(ACPI::SDTHeader), &s3a, &s3b)){
                    haveS3 = TRUE;
                    Printk::Write(Printk::Level::LOG_INFO, "[Power] Parsed ACPI _S3_: SLP_TYPa=%u SLP_TYPb=%u\n", (unsigned)s3a, (unsigned)s3b);
                } else {
                    Printk::Write(Printk::Level::LOG_INFO, "[Power] ACPI _S3_ not found in DSDT\n");
                }
                // Keep mapping; system will power off. If not, we could free.
            } while(0);

            if (haveS5 && ACPI::g_Pm1aControlPort) {
                // Try to enable ACPI first, if SMI CMD present
                if (ACPI::g_SmiCommandPort && ACPI::g_AcpiEnableValue) {
                    Port::Outb((U16)ACPI::g_SmiCommandPort, (U8)ACPI::g_AcpiEnableValue);
                }
                const U16 SLP_EN = 1u << 13; // common ACPI
                const U16 SLP_TYP_SHIFT = 10;
                U16 valA = (U16)((s5a << SLP_TYP_SHIFT) | SLP_EN);
                Printk::Write(Printk::Level::LOG_INFO, "[Power] ACPI S5: PM1a=0x%04x val=0x%04x (TYPa=%u)\n", (unsigned)ACPI::g_Pm1aControlPort, (unsigned)valA, (unsigned)s5a);
                Port::Outw((U16)ACPI::g_Pm1aControlPort, valA);
                if (ACPI::g_Pm1bControlPort) {
                    U16 valB = (U16)((s5b << SLP_TYP_SHIFT) | SLP_EN);
                    Printk::Write(Printk::Level::LOG_INFO, "[Power] ACPI S5: PM1b=0x%04x val=0x%04x (TYPb=%u)\n", (unsigned)ACPI::g_Pm1bControlPort, (unsigned)valB, (unsigned)s5b);
                    Port::Outw((U16)ACPI::g_Pm1bControlPort, valB);
                }
            }

            // Fallbacks widely supported by QEMU/Bochs/i440fx chipsets
            Port::Outw(0x604, 0x2000);
            Port::Outw(0xB004, 0x2000);
            Port::Outw(0x4004, 0x3400);

            Printk::Write(Printk::Level::LOG_EMERG, "[Power] ACPI shutdown failed, halting\n");
            for(;;) asm volatile ("hlt");
        }
    }
}
