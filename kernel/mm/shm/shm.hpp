#pragma once

#include "string.hpp"
#include <rosval.h>
#include <mm.hpp>
#include <filesystem/filesystem.hpp>

// Struct Region yang udah ditambahin Inode (ide PrivateData lu tadi)
struct ShmRegion {
    char Name[64];
    UPTR PhysAddr;    // Alamat Fisik Halaman Pertama
    U64 SizeInPages;
    U32 RefCount;
    BOOL Used;
    Inode* RegionInode; // <--- Inode tunggal milik fisik ini
};

class SharedMemoryManager {
    private:
        // Pindahin array-nya ke .cpp aja biar bener-bener private, 
        // atau deklarasi doang di sini. Kita pakai statik di .cpp aja biar aman.
    public:
        static VOID Init();
        static ShmRegion* Open(const CHAR8* Name, U64 Size, BOOL CreateNew);
        static VOID Release(ShmRegion* Region);
};