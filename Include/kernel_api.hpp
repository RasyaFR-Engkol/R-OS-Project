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
    VOID *(*VirtualFreePages)(VOID* addr, SIZE_T count);

    //PAGINGPHYSICAL
    UPTR (*PhysicalAllocLowPages)(SIZE_T count);
    VOID (*PhysicalFreeLowPages)(UPTR addr, SIZE_T count);
    UPTR (*PhysicalAllocPages)(SIZE_T count);
    VOID (*PhysicalFreePages)(UPTR addr, SIZE_T count);

    //KMALLOC
    VOID* (*Alloc)(SIZE_T size);
    VOID (*Free)(VOID* ptr);
};

typedef int (*ModuleInitFunc)(KernelAPI *API);