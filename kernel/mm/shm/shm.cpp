#include <rosval.h>
#include "shm.hpp"

static ShmRegion ShmRegions[64];

VOID SharedMemoryManager::Init(){
    for(INTN i = 0; i < 64; i++){
        ShmRegions[i].Used = FALSE;
        String::Memset(ShmRegions[i].Name, 0, sizeof(ShmRegions[i].Name));
        ShmRegions[i].RegionInode = nullptr;
    }
}

ShmRegion *SharedMemoryManager::Open(const CHAR8 *Name, U64 Size, BOOL CreateNew){
    // 1. Coba cari yang udah ada
    for(INTN i = 0; i < 64; i++){
        if(ShmRegions[i].Used && String::Strcmp(ShmRegions[i].Name, Name) == 0){
            ShmRegions[i].RefCount++;
            return &ShmRegions[i];
        }
    }

    if(!CreateNew) return nullptr;

    // 2. Bikin baru
    for(INTN i = 0; i < 64; i++){
        if(!ShmRegions[i].Used){
            U64 Pages = (Size + PAGE_SIZE - 1) / PAGE_SIZE;
            UPTR Phys = PageAlloc::PhysicalAllocPages(Pages);

            if(!Phys) return nullptr;

            // FIX WARNA-WARNI DI SINI
            // Ambil pointer virtual kernel via HHDM (Higher Half Direct Mapping)
            // Lalu isi memori fisiknya dengan 0 (Hitam/Kosong)
            void* KernelPtr = HHDM_PhysToVirt(Phys);
            String::Memset(KernelPtr, 0xAA, Pages * PAGE_SIZE); 

            // Setup Data Region
            String::Strcpy(ShmRegions[i].Name, Name);
            ShmRegions[i].PhysAddr = Phys;
            ShmRegions[i].RefCount = 1;
            ShmRegions[i].Used = TRUE;
            ShmRegions[i].SizeInPages = Pages;
            
            // Bikin Inode tunggal buat VFS
            ShmRegions[i].RegionInode = new Inode();
            ShmRegions[i].RegionInode->InodeID = (U64)&ShmRegions[i]; // Unik
            ShmRegions[i].RegionInode->FileSize = Pages * PAGE_SIZE;

            return &ShmRegions[i];
        }
    }
    return nullptr;
}

VOID SharedMemoryManager::Release(ShmRegion* Region) {
    if (!Region || !Region->Used) return;

    Region->RefCount--;

    if (Region->RefCount <= 0) {
        // Bebasin RAM
        PageAlloc::PhysicalFreePages(Region->PhysAddr, Region->SizeInPages);
        
        // Hapus Inode
        if (Region->RegionInode) {
            delete Region->RegionInode;
            Region->RegionInode = nullptr;
        }

        // Reset Struct
        Region->Used = FALSE;
        Region->PhysAddr = 0;
        Region->RefCount = 0;
        String::Memset(Region->Name, 0, sizeof(Region->Name));
    }
}