#pragma once
#include <rosval.h>
#include <logging.hpp>
#include <mm.hpp>

struct KernelAPI{
    // LOGGING
    BOOL (*Printk)(Printk::Level Level, const char* Format, ...);
    VOID (*Panic)(const char* Format, ...);

    //DMA
    PageAlloc::DMAAlloc::DMABuffer *(*AllocateDMAPages)(SIZE_T count);
    PageAlloc::DMAAlloc::DMABuffer *(*AllocateDMABytes)(SIZE_T bytes);
    VOID (*FreeDMAPages)(UPTR addr, SIZE_T count);

    //PAGINGVIRTUAL
    VOID *(*VirtualAllocPages)(SIZE_T count);
    VOID (*VirtualFreePages)(VOID* addr, SIZE_T count);

    //PAGINGPHYSICAL
    UPTR (*PhysicalAllocLowPages)(SIZE_T count);
    VOID (*PhysicalFreeLowPages)(UPTR addr, SIZE_T count);
    UPTR (*PhysicalAllocPages)(SIZE_T count);
    VOID (*PhysicalFreePages)(UPTR addr, SIZE_T count);

    //KMALLOC
    VOID* (*Alloc)(SIZE_T size);
    VOID (*Free)(VOID* ptr);
};

typedef int (*ModuleInitFunc)(VOID* PrivateData);

ABI_C {
    // api from any kernel function
    // PCI
    U32 PCIReadDword(U8 Bus, U8 Device, U8 Function, U8 Offset);
    VOID PCIWriteDword(U8 Bus, U8 Device, U8 Function, U8 Offset, U32 Value);
    U16 PCIReadWord(U8 Bus, U8 Device, U8 Function, U8 Offset);
    VOID PCIWriteWord(U8 Bus, U8 Device, U8 Function, U8 Offset, U16 Value);
    U8 PCIReadByte(U8 Bus, U8 Device, U8 Function, U8 Offset);
    VOID PCIWriteByte(U8 Bus, U8 Device, U8 Function, U8 Offset, U8 Value);
    U8 PCIFindCapatibility(U8 Bus, U8 Device, U8 Function, U8 TargetCapID);
    U8 PCIEnableMSI(U8 bus, U8 dev, U8 func, U8 msi_cap_offset, void (*handler)(void *context));
    U8 PCIEnableMSIX(U8 bus, U8 dev, U8 func, U8 msix_cap_offset, void (*handler)(void *context));
    U8 PCIEnableLegacyINTx(U8 Bus, U8 Device, U8 Function, void (*irq_handler)(void *context));

    // Memory management
    VOID* MmVirtualAllocPages(SIZE_T count);
    VOID MmVirtualFreePages(VOID* addr, SIZE_T count);
    BOOL MmMapPages(U64 *PML4Virt, UPTR PhysAddr, UPTR VirtAddr, SIZE_T Count, U64 Flags);
    BOOL MmUnMapPages(U64 *PML4Virt, UPTR VirtAddr);

    //Threaded IRQ
    VOID RequestThreadedIrq(U8 Vector, VOID (*TopHalf)(VOID* Ctx1), VOID (*BottomHalf)(VOID*), VOID *DevID);
    VOID WakeUpThreadedIrq(U8 Vector);

    // Device Manager
    class IBlockDevice;
    class ICharDevice;

    BOOL DMRegisterBlockDevice(IBlockDevice *Device);
    BOOL DMRegisterCharDevice(ICharDevice *Device);

    // VFS
    BOOL VFSResolvePath(const char *path, FileSystem **outFS, char *OutRelativePath, BOOL FollowLastSymlink = FALSE);
    BOOL VFSCreateBlockNode(IBlockDevice *dev);
}