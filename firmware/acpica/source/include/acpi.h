#pragma once

#include <stdint.h>
#include <stddef.h>
#include <rosval.h>

/* Basic integer types used by ACPICA-style code */
typedef uint8_t  UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef char*    ACPI_STRING;

typedef int BOOLEAN;

/* Common ACPICA enums/callbacks (minimal) */
typedef void (*ACPI_OSD_HANDLER)(void);
typedef void (*ACPI_OSD_EXEC_CALLBACK)(void*);
typedef uint64_t ACPI_THREAD_ID;
typedef enum { ACPI_EX_NONE = 0 } ACPI_EXECUTE_TYPE;

typedef struct { /* placeholder for predefined names */ } ACPI_PREDEFINED_NAMES;

/* Minimal compatibility header to replace full ACPICA headers when ACPICA is removed.
 * This provides only the lightweight typedefs and macros used by our OSL shim and
 * some ACPI-using modules in the kernel. It is intentionally small and not a
 * replacement for the full ACPICA API.
 */

typedef int ACPI_STATUS;
#define AE_OK 0
#define AE_NO_MEMORY 1
#define AE_NOT_FOUND 2
#define AE_BAD_PARAMETER 3
#define AE_NOT_IMPLEMENTED 4

#define ACPI_SUCCESS(s) ((s) == AE_OK)
#define ACPI_FAILURE(s) (!ACPI_SUCCESS(s))

typedef uint64_t ACPI_PHYSICAL_ADDRESS;
typedef size_t ACPI_SIZE;

typedef void* ACPI_SPINLOCK;
typedef uint64_t ACPI_CPU_FLAGS;
typedef void* ACPI_SEMAPHORE;

typedef uint32_t ACPI_IO_ADDRESS;

typedef struct { uint8_t Bus; uint8_t Device; uint8_t Function; uint8_t Padding; } ACPI_PCI_ID;

/* Placeholder macros used in some ACPICA code */
#define ACPI_INTERNAL_VAR_XFACE

/* Minimal printf-like facility placeholder (real OSL provides AcpiOsPrintf) */

/* Forward declare table header type used by some callers */
typedef struct ROS_PACKED {
    char Signature[4];
    uint32_t Length;
    uint8_t Revision;
    uint8_t Checksum;
    char OemId[6];
    char OemTableId[8];
    uint32_t OemRevision;
    uint32_t CreatorId;
    uint32_t CreatorRevision;
} ACPI_TABLE_HEADER;

/* Minimal BGRT table used by our BGRT helper */
typedef struct ROS_PACKED{
    ACPI_TABLE_HEADER Header;
    UINT8 Version;
    UINT8 Status;
    UINT8 ImageType;
    UINT8 Reserved;
    UINT64 ImageAddress;
    UINT32 ImageOffsetX;
    UINT32 ImageOffsetY;
} ACPI_TABLE_BGRT;

/* Minimal MCFG allocation entry */
typedef struct ROS_PACKED{
    UINT64 Address;
    UINT16 PciSegment;
    UINT8 StartBusNumber;
    UINT8 EndBusNumber;
} ACPI_MCFG_ALLOCATION;

typedef struct ROS_PACKED{
    ACPI_TABLE_HEADER Header;
    UINT32 Flags;
} ACPI_TABLE_WAET;


/* Signatures used by older code */
#define ACPI_SIG_FADT "FACP"
#define ACPI_SIG_BGRT "BGRT"
#define ACPI_SIG_MCFG "MCFG"

/* Provide empty prototypes so code that links to these symbols compiles —
 * we'll not use ACPICA library, so these are just stubs (not linked).
 */
#ifdef __cplusplus
extern "C" {
#endif

ACPI_STATUS AcpiInitializeSubsystem(void);
ACPI_STATUS AcpiInitializeTables(void* A, int B, int C);
ACPI_STATUS AcpiLoadTables(void);
ACPI_STATUS AcpiEnableSubsystem(unsigned long Flags);
ACPI_STATUS AcpiInitializeObjects(unsigned long Flags);
ACPI_STATUS AcpiGetTable(const char* Signature, unsigned Index, ACPI_TABLE_HEADER** Out);

#ifdef __cplusplus
}
#endif
