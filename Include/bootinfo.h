#ifndef BOOTINFO_H
#define BOOTINFO_H

#include "serial.hpp"
#include <rosval.h>

#define REQ_MULTIBOOT_GET_INFO 0x01

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BootFramebufferInfo {
    U64 address;
    U32 pitch;
    U32 width;
    U32 height;
    U8 bpp;
    U8 type;
    U8 reserved0[2];
    U8 red_position;
    U8 red_mask_size;
    U8 green_position;
    U8 green_mask_size;
    U8 blue_position;
    U8 blue_mask_size;
} BootFramebufferInfo;

typedef struct BootAcpiInfo {
    const void *rsdp;
    U32 length;
    U8 revision;
    U8 is_xsdp;
    U8 reserved1[6];
} BootAcpiInfo;

typedef struct BootMemRegion {
    U64 base;
    U64 length;
    U32 type;     // 1=available, others reserved/ACPI/etc
    U32 reserved; // alignment
} BootMemRegion;

typedef struct BootMemMap {
    BootMemRegion regions[128];
    U32 count;
    U32 entry_size;
    U32 entry_version;
} BootMemMap;

typedef struct BootTagTrace {
    U32 types[32];
    U32 count;
} BootTagTrace;

typedef struct BootInfo {
    U64 multiboot_addr;
    U32 multiboot_total_size;
    BootFramebufferInfo framebuffer;
    BootAcpiInfo acpi;
    BootMemMap memmap;
    BootTagTrace tag_trace;
    BOOL has_framebuffer;
    BOOL has_acpi;
    BOOL has_memmap;
} BootInfo;

const BootInfo *BootInfoGet(void);

static VOID BootInfoPrint(){
    const BootInfo *boot = BootInfoGet();
    if (boot) {
        Serial::Printf("[ROS] BootInfo: mb@%p fb=%s acpi=%s\n",
            (void*)(uintptr_t)boot->multiboot_addr,
            boot->has_framebuffer ? "yes" : "no",
            boot->has_acpi ? "yes" : "no");

        Serial::Printf("[ROS] MB tags (%u):", (unsigned)boot->tag_trace.count);
        for (U32 i = 0; i < boot->tag_trace.count; ++i) {
            Serial::Printf(" %u", (unsigned)boot->tag_trace.types[i]);
        }
        Serial::Write("\n");

        if (boot->has_framebuffer) {
            Serial::Printf(
                "[ROS] FB addr=%p pitch=%u res=%ux%u bpp=%u type=%u\n",
                (void*)(uintptr_t)boot->framebuffer.address,
                (unsigned)boot->framebuffer.pitch,
                (unsigned)boot->framebuffer.width,
                (unsigned)boot->framebuffer.height,
                (unsigned)boot->framebuffer.bpp,
                (unsigned)boot->framebuffer.type);

            Serial::Printf(
                "[ROS] FB RGB pos=%u/%u/%u mask=%u/%u/%u\n",
                (unsigned)boot->framebuffer.red_position,
                (unsigned)boot->framebuffer.green_position,
                (unsigned)boot->framebuffer.blue_position,
                (unsigned)boot->framebuffer.red_mask_size,
                (unsigned)boot->framebuffer.green_mask_size,
                (unsigned)boot->framebuffer.blue_mask_size);
        }

        if (boot->has_acpi) {
            const void *acpi_ptr = boot->acpi.rsdp;
            const char *acpi_kind = boot->acpi.is_xsdp ? "RSDP (ACPI >=2.0, XSDP - contains XSDT)" : "RSDP (ACPI 1.0, points to RSDT)";
            Serial::Printf("[ROS] ACPI ptr=%p len=%u rev=%u kind=%s\n",
                acpi_ptr,
                (unsigned)boot->acpi.length,
                (unsigned)boot->acpi.revision,
                acpi_kind);
        }

        if (boot->has_memmap) {
            Serial::Printf("[ROS] E820 entries (%u) size=%u ver=%u\n",
                (unsigned)boot->memmap.count,
                (unsigned)boot->memmap.entry_size,
                (unsigned)boot->memmap.entry_version);
            for (U32 i = 0; i < boot->memmap.count; ++i) {
                const BootMemRegion *r = &boot->memmap.regions[i];
                const char *type = "RES";
                switch (r->type) {
                    case 1: type = "RAM"; break;
                    case 2: type = "ACPI"; break;
                    case 3: type = "ACPI_NVS"; break;
                    case 4: type = "BAD"; break;
                    case 5: type = "PERSIST"; break;
                }
                /* Print start and end (inclusive) addresses for easier reading.
                 * Protect against zero-length entries when computing end. */
                Serial::Printf("  [ROS] E820: %s start=%p end=%p\n",
                    type,
                    (void*)(uintptr_t)r->base,
                    (void*)(uintptr_t)(r->length ? (r->base + r->length - 1) : r->base));
            }
        }
    }
}

#ifdef __cplusplus
}
#endif

#endif
