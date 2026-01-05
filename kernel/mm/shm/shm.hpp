#pragma once

#include "string.hpp"
#include <rosval.h>
#include <mm.hpp>
#include <filesystem/filesystem.hpp>

struct ShmRegion {
    char Name[64];
    UPTR PhysAddr;    // Alamat Fisik Halaman Pertama
    U64 SizeInPages;
    U32 RefCount;
    BOOL Used;
};

class SharedMemoryManager{
    private:
        inline static ShmRegion ShmRegions[64];
    public:
        static VOID Init(){
            for(INTN i = 0; i < 64; i++){
                ShmRegions[i].Used = FALSE;
                String::Memset(ShmRegions[i].Name, 0, sizeof(ShmRegions[i].Name));
            }
        }

        // Membuka ShmRegion (Lebih tepatnya mencari jika CreateNew false).
        // Jika ketemu, return SHMRegion dan naikan RefCount. Tapi jika 
        // tidak ketemu tapi CreateNew nya false, return nullptr. Tapi jika 
        // tidak ketemu dan CreateNew nya TRUE, buat SHM, naikan refcount,
        // kembalikan ke pembuat
        // 
        // Biasanya dipanggil oleh:
        // Sys_ShmOpen
        static ShmRegion *Open(const CHAR8* Name, U64 Size, BOOL CreateNew){
            for(INTN i = 0; i < 64; i++){
                if(ShmRegions[i].Used && String::Strcmp(ShmRegions[i].Name, Name) == 0){
                    ShmRegions[i].RefCount++;
                    //Printk::Write(Printk::Level::LOG_DINFO, "SHMRegion: Opening Region Refcount: %d. Region name: %s.\n", ShmRegions[i].RefCount, ShmRegions[i].Name);
                    return &ShmRegions[i];
                }
            }

            if(!CreateNew) return nullptr;

            for(INTN i = 0; i < 64; i++){
                if(!ShmRegions[i].Used){
                    U64 Pages = (Size + PAGE_SIZE - 1) / PAGE_SIZE;
                    UPTR Phys = PageAlloc::PhysicalAllocPages(Pages);

                    if(!Phys) return nullptr;

                    String::Strcpy(ShmRegions[i].Name, Name);
                    ShmRegions[i].PhysAddr = Phys;
                    ShmRegions[i].RefCount = 1;
                    ShmRegions[i].Used = TRUE;
                    ShmRegions[i].SizeInPages = Pages;

                    //Printk::Write(Printk::Level::LOG_DINFO, "SHMRegion: Creating Region Refcount: %d. Region name: %s.\n", ShmRegions[i].RefCount, ShmRegions[i].Name);

                    return &ShmRegions[i];
                }
            }
            return nullptr;
        }

        static VOID Release(ShmRegion* Region) {
            if (!Region || !Region->Used) {
                //Printk::Write(Printk::Level::LOG_DERR, "SHMRegion: Region not used or no region.\n");
                return;
            }

            Region->RefCount--;

            //Printk::Write(Printk::Level::LOG_DINFO, "SHMRegion: Region Refcount: %d. Region name: %s.\n", Region->RefCount, Region->Name);

            // Kalau gak ada lagi yang pake, balikin RAM ke negara
            if (Region->RefCount <= 0) {
                PageAlloc::PhysicalFreePages(Region->PhysAddr, Region->SizeInPages);
                
                // Reset Struct
                Region->Used = FALSE;
                Region->PhysAddr = 0;
                Region->RefCount = 0;
                String::Memset(Region->Name, 0, sizeof(Region->Name));
                
                //Printk::Write(Printk::Level::LOG_DOK, "SHM Freed: Region released.\n");
            } else {
                //Printk::Write(Printk::Level::LOG_WARNING, "SHM Region: Refcount above zero. Can't release.\n");
            }
        }
};

struct ShmFile : public File{
    ShmRegion *Region;

    ShmFile(ShmRegion *R){
        Region = R;
        type = FileType::FT_SHM;
        FileSize = 0;
        CurrentPosition = 0;
        IsDirectory = FALSE;
        Internal_CurrentCluster = 0;
        Internal_StartCluster = 0;
        Internal_DirEntryLBA = 0;
        Internal_DirEntryOffset = 0;
        RefCount = 1; 
        FSOwner = nullptr; // No filesystem owner for anonymous shm files
        String::Memset(FileName, 0, sizeof(FileName));
        if (Region && Region->Name[0] != '\0') {
            String::Strcpy(FileName, Region->Name);
        }
        // Set file size to region size in bytes
        if (Region) FileSize = (U64)Region->SizeInPages * PAGE_SIZE;
    }

    ~ShmFile() override {
        // Panggil manager buat release refcount fisik
        if(Region){
            SharedMemoryManager::Release(Region);
            Region = nullptr;
        }
    }
};