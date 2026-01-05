// Minimal ACPICA OSL (OS Services Layer) for ROS kernel
// Compiled as C++ but exports C symbols for ACPICA

#define PRINTK_MODULE_NAME "ACPICA-OSL"

// Include minimal ACPICA public header. Define environment hints before include.
extern "C" {
    #include "../acpica/source/include/acpi.h"
}

#include <rosval.h>
#include <bootinfo.h>
#include <logging.hpp>
#include <mm.hpp>
#include <port.hpp>
#include <string.hpp>
#include <rossys.hpp>
#include <drivers/pci.hpp>

using namespace Printk;
using namespace String;

extern "C" {

ACPI_STATUS AcpiOsInitialize(void) {
    Write(Level::LOG_INFO, "OSL init\n");
    return AE_OK;
}

ACPI_PHYSICAL_ADDRESS AcpiOsGetRootPointer(void) {
    // Use BootInfo RSDP physical pointer
    const BootInfo* bi = BootInfoGet();
    if (!bi || !bi->has_acpi || !bi->acpi.rsdp) return 0;
    UPTR rsdp_phys = (UPTR)bi->acpi.rsdp;
    if (rsdp_phys >= (UPTR)HHDM_BASE) {
        // Stored as virtual HHDM pointer, convert back to phys
        rsdp_phys -= (UPTR)HHDM_BASE;
    }
    return (ACPI_PHYSICAL_ADDRESS)rsdp_phys;
}

ACPI_STATUS AcpiOsTerminate(void) {
    Write(Level::LOG_INFO, "OSL term\n");
    return AE_OK;
}

void *AcpiOsAllocate(ACPI_SIZE Size) {
    void* p = Kmalloc::Alloc((SIZE_T)Size);
    // Optional: zero callers should use AllocateZeroed
    return p;
}

/* Do not provide AcpiOsAllocateZeroed to avoid duplicate if ACPICA provides one. */

void AcpiOsFree(void *Memory) {
    if (Memory) Kmalloc::Free(Memory);
}

void *AcpiOsMapMemory(ACPI_PHYSICAL_ADDRESS Where, ACPI_SIZE Length) {
    (void)Length;
    // ROS maps all physical memory via HHDM, so just translate
    return (void*)HHDM_PhysToVirt((UPTR)Where);
}

void AcpiOsUnmapMemory(void *LogicalAddress, ACPI_SIZE Size) {
    (void)LogicalAddress; (void)Size;
    // HHDM mapping is permanent; nothing to do
}

ACPI_STATUS AcpiOsGetPhysicalAddress(void *LogicalAddress, ACPI_PHYSICAL_ADDRESS *PhysicalAddress) {
    if (!PhysicalAddress) return AE_BAD_PARAMETER;
    // HHDM: phys = virt - HHDM_BASE
    *PhysicalAddress = (ACPI_PHYSICAL_ADDRESS)((UPTR)LogicalAddress - (UPTR)HHDM_BASE);
    return AE_OK;
}

// Spinlocks — implement with our simple spinlock + interrupt save/restore
typedef struct { SPINLOCK_T L; } ROS_ACPI_LOCK;

ACPI_STATUS AcpiOsCreateLock(ACPI_SPINLOCK *OutHandle) {
    if (!OutHandle) return AE_BAD_PARAMETER;
    ROS_ACPI_LOCK* lk = (ROS_ACPI_LOCK*)Kmalloc::Alloc(sizeof(ROS_ACPI_LOCK));
    if (!lk) return AE_NO_MEMORY;
    Arch::Spinlock::SpinlockInit(&lk->L);
    *OutHandle = (ACPI_SPINLOCK)lk;
    return AE_OK;
}

void AcpiOsDeleteLock(ACPI_SPINLOCK Handle) {
    if (!Handle) return;
    Kmalloc::Free((void*)Handle);
}

ACPI_CPU_FLAGS AcpiOsAcquireLock(ACPI_SPINLOCK Handle) {
    if (!Handle) return 0;
    LOCKRFLAGS flags = Arch::SaveAndDisableInterrupts();
    ROS_ACPI_LOCK* lk = (ROS_ACPI_LOCK*)Handle;
    Arch::Spinlock::SpinLockAcquire(&lk->L);
    return (ACPI_CPU_FLAGS)flags;
}

void AcpiOsReleaseLock(ACPI_SPINLOCK Handle, ACPI_CPU_FLAGS Flags) {
    if (!Handle) return;
    ROS_ACPI_LOCK* lk = (ROS_ACPI_LOCK*)Handle;
    Arch::Spinlock::SpinLockRelease(&lk->L);
    Arch::RestoreInterrupts((LOCKRFLAGS)Flags);
}

// Semaphores — simple counter + busy-wait for now (single-threaded kernel)
typedef struct { UINT32 max; volatile UINT32 count; } ROS_ACPI_SEM;

ACPI_STATUS AcpiOsCreateSemaphore(UINT32 MaxUnits, UINT32 InitialUnits, ACPI_SEMAPHORE *OutHandle) {
    if (!OutHandle || InitialUnits > MaxUnits) return AE_BAD_PARAMETER;
    ROS_ACPI_SEM* s = (ROS_ACPI_SEM*)Kmalloc::Alloc(sizeof(ROS_ACPI_SEM));
    if (!s) return AE_NO_MEMORY;
    s->max = MaxUnits; s->count = InitialUnits;
    *OutHandle = (ACPI_SEMAPHORE)s;
    return AE_OK;
}

ACPI_STATUS AcpiOsDeleteSemaphore(ACPI_SEMAPHORE Handle) {
    if (!Handle) return AE_BAD_PARAMETER;
    Kmalloc::Free((void*)Handle);
    return AE_OK;
}

ACPI_STATUS AcpiOsWaitSemaphore(ACPI_SEMAPHORE Handle, UINT32 Units, UINT16 Timeout) {
    (void)Timeout; // busy-wait; TODO: add timed wait if needed
    ROS_ACPI_SEM* s = (ROS_ACPI_SEM*)Handle;
    if (!s || Units == 0) return AE_BAD_PARAMETER;
    while (true) {
        // naive spin: try acquire
        UINT32 c = s->count;
        if (c >= Units) { s->count = c - Units; return AE_OK; }
        Arch::CPURelax();
    }
}

ACPI_STATUS AcpiOsSignalSemaphore(ACPI_SEMAPHORE Handle, UINT32 Units) {
    ROS_ACPI_SEM* s = (ROS_ACPI_SEM*)Handle;
    if (!s || Units == 0) return AE_BAD_PARAMETER;
    UINT32 newc = s->count + Units;
    if (newc > s->max) newc = s->max;
    s->count = newc;
    return AE_OK;
}

// Time
void AcpiOsSleep(UINT64 Milliseconds) { Arch::Time::SleepMs(Milliseconds); }
void AcpiOsStall(UINT32 Microseconds) {
    // Convert to ms (ceil) and reuse SleepMs; sub-ms precision not needed now
    UINT64 ms = (Microseconds + 999) / 1000ULL;
    if (ms == 0) ms = 1;
    Arch::Time::SleepMs(ms);
}

/* Optional signaling (used by some exec paths). Stub until eventing added. */
ACPI_STATUS AcpiOsSignal(UINT32 Function, void *Info) {
    (void)Function; (void)Info; return AE_OK;
}

UINT64 AcpiOsGetTimer(void) {
    // Return in 100-ns units as per ACPICA contract
    // PIT tick ~= (1 / TickHz) seconds
    const UINT64 hz = (UINT64)Arch::Time::TickHz();
    const UINT64 ticks = (UINT64)Arch::Time::NowTicks();
    if (hz == 0) return 0;
    // 1 second = 10,000,000 x 100ns
    return (ticks * 10000000ULL) / hz;
}

// I/O ports
ACPI_STATUS AcpiOsReadPort(ACPI_IO_ADDRESS Address, UINT32 *Value, UINT32 Width) {
    if (!Value) return AE_BAD_PARAMETER;
    switch (Width) {
        case 8:  *Value = Port::Inb((U16)Address); break;
        case 16: *Value = Port::Inw((U16)Address); break;
        case 32: *Value = Port::Inl((U16)Address); break;
        default: return AE_BAD_PARAMETER;
    }
    return AE_OK;
}

ACPI_STATUS AcpiOsWritePort(ACPI_IO_ADDRESS Address, UINT32 Value, UINT32 Width) {
    switch (Width) {
        case 8:  Port::Outb((U16)Address, (U8)Value); break;
        case 16: Port::Outw((U16)Address, (U16)Value); break;
        case 32: Port::Outl((U16)Address, (U32)Value); break;
        default: return AE_BAD_PARAMETER;
    }
    return AE_OK;
}

// Physical memory I/O (MMIO)
ACPI_STATUS AcpiOsReadMemory(ACPI_PHYSICAL_ADDRESS Address, UINT64 *Value, UINT32 Width) {
    if (!Value) return AE_BAD_PARAMETER;
    UPTR va = (UPTR)HHDM_PhysToVirt((UPTR)Address);
    switch (Width) {
        case 8:  *Value = (UINT64)MMIOPort::Read8(va); break;
        case 16: *Value = (UINT64)MMIOPort::Read16(va); break;
        case 32: *Value = (UINT64)MMIOPort::Read32(va); break;
        case 64: *Value = (UINT64)MMIOPort::Read64(va); break;
        default: return AE_BAD_PARAMETER;
    }
    return AE_OK;
}

ACPI_STATUS AcpiOsWriteMemory(ACPI_PHYSICAL_ADDRESS Address, UINT64 Value, UINT32 Width) {
    UPTR va = (UPTR)HHDM_PhysToVirt((UPTR)Address);
    switch (Width) {
        case 8:  MMIOPort::Write8(va, (U8)Value); break;
        case 16: MMIOPort::Write16(va, (U16)Value); break;
        case 32: MMIOPort::Write32(va, (U32)Value); break;
        case 64: MMIOPort::Write64(va, (U64)Value); break;
        default: return AE_BAD_PARAMETER;
    }
    return AE_OK;
}

// PCI config space access via existing PCI code
ACPI_STATUS AcpiOsReadPciConfiguration(ACPI_PCI_ID *PciId, UINT32 Reg, UINT64 *Value, UINT32 Width) {
    if (!PciId || !Value) return AE_BAD_PARAMETER;
    U8 bus = (U8)PciId->Bus; U8 dev = (U8)PciId->Device; U8 fun = (U8)PciId->Function;
    // Our helpers are DWORD-based; read 32 then mask/shift
    U32 d = PCI::ReadDword(bus, dev, fun, (U8)Reg);
    switch (Width) {
        case 8:  *Value = (d >> ((Reg & 3) * 8)) & 0xFFu; break;
        case 16: *Value = (d >> ((Reg & 2) * 8)) & 0xFFFFu; break;
        case 32: *Value = d; break;
        default: return AE_BAD_PARAMETER;
    }
    return AE_OK;
}

ACPI_STATUS AcpiOsWritePciConfiguration(ACPI_PCI_ID *PciId, UINT32 Reg, UINT64 Value, UINT32 Width) {
    if (!PciId) return AE_BAD_PARAMETER;
    U8 bus = (U8)PciId->Bus; U8 dev = (U8)PciId->Device; U8 fun = (U8)PciId->Function;
    if (Width == 32 && (Reg % 4 == 0)) {
        PCI::WriteDword(bus, dev, fun, (U8)Reg, (U32)Value);
        return AE_OK;
    }
    // For 8/16-bit writes, read-modify-write the 32-bit dword
    U8 aligned = (U8)(Reg & ~3u);
    U32 d = PCI::ReadDword(bus, dev, fun, aligned);
    switch (Width) {
        case 8: {
            U8 shift = (Reg & 3u) * 8u;
            d &= ~(0xFFu << shift);
            d |= ((U32)(U8)Value) << shift;
            break;
        }
        case 16: {
            U8 shift = (Reg & 2u) * 8u;
            d &= ~(0xFFFFu << shift);
            d |= ((U32)(U16)Value) << shift;
            break;
        }
        default: return AE_BAD_PARAMETER;
    }
    PCI::WriteDword(bus, dev, fun, aligned, d);
    return AE_OK;
}

// Misc
BOOLEAN AcpiOsReadable(void *Pointer, ACPI_SIZE Length) { (void)Pointer; (void)Length; return TRUE; }
BOOLEAN AcpiOsWritable(void *Pointer, ACPI_SIZE Length) { (void)Pointer; (void)Length; return TRUE; }

ACPI_STATUS AcpiOsPredefinedOverride(const ACPI_PREDEFINED_NAMES *InitVal, ACPI_STRING *NewVal) {
    (void)InitVal; if (NewVal) *NewVal = NULL; return AE_OK;
}
ACPI_STATUS AcpiOsTableOverride(ACPI_TABLE_HEADER *ExistingTable, ACPI_TABLE_HEADER **NewTable) {
    (void)ExistingTable; if (NewTable) *NewTable = NULL; return AE_OK;
}
ACPI_STATUS AcpiOsPhysicalTableOverride(ACPI_TABLE_HEADER *ExistingTable, ACPI_PHYSICAL_ADDRESS *NewAddress, UINT32 *NewTableLength) {
    (void)ExistingTable; (void)NewTableLength; if (NewAddress) *NewAddress = 0; return AE_OK;
}

// Interrupt handler install/remove — not wired yet, return unsupported
ACPI_STATUS AcpiOsInstallInterruptHandler(UINT32 InterruptNumber, ACPI_OSD_HANDLER ServiceRoutine, void *Context) {
    (void)InterruptNumber; (void)ServiceRoutine; (void)Context; return AE_NOT_IMPLEMENTED;
}
ACPI_STATUS AcpiOsRemoveInterruptHandler(UINT32 InterruptNumber, ACPI_OSD_HANDLER ServiceRoutine) {
    (void)InterruptNumber; (void)ServiceRoutine; return AE_NOT_IMPLEMENTED;
}

// Threads — single-threaded for now
ACPI_THREAD_ID AcpiOsGetThreadId(void) { return (ACPI_THREAD_ID)1; }
ACPI_STATUS AcpiOsExecute(ACPI_EXECUTE_TYPE Type, ACPI_OSD_EXEC_CALLBACK Function, void *Context) {
    (void)Type; if (Function) Function(Context); return AE_OK;
}
void AcpiOsWaitEventsComplete(void) { }

// Debug print bridge
// ACPICA may pass varargs that contain pointers into ACPI/AML space. Those
// pointers can be small/invalid (in some virtualization environments) and
// dereferencing them in the kernel print routines can cause page faults
// (observed as derefs to addresses like 0x3). To avoid crashing the system
// during ACPI table/method evaluation, make the OSL debug bridge conservative
// and avoid forwarding raw varargs directly into the kernel formatter.
//
// Short-term safe behavior: print the format string itself (literal) and do
// not attempt to expand the varargs. This preserves a minimal amount of
// debug information while preventing crashes. If you need full formatted
// ACPI debug output later, implement a safe formatter that validates any
// pointer arguments before dereferencing.
void ACPI_INTERNAL_VAR_XFACE AcpiOsPrintf(const char *Format, ...) {
    (void)Format;
    // Print the format string only (safe). If Format is NULL, show a marker.
    const char* fmt = Format ? Format : "(null)";
    Printk::Write(Printk::Level::LOG_INFO, "[ACPICA] %s", fmt);
}

void AcpiOsVprintf(const char *Format, va_list Args) {
    (void)Args;
    const char* fmt = Format ? Format : "(null)";
    Printk::Write(Printk::Level::LOG_INFO, "[ACPICA] %s", fmt);
}

void AcpiOsRedirectOutput(void *Destination) { (void)Destination; }

}
