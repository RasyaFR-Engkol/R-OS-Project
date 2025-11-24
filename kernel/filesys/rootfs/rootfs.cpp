#include "rootfs.hpp"
#include "rosval.h"
#include <string.hpp>
#include <mm.hpp> // Untuk Kmalloc/Kfree atau new/delete
#define PRINTK_MODULE_NAME "RootFS"
#include <logging.hpp>
#include <filesystem/linux_dirent.hpp>
#include <filesystem/filesystem.hpp>
#include "../vfs/vfs.hpp"

namespace ROOTFS{
    VOID InitROOTFS(){
        RootFS *MyROOT = new RootFS();
        VFSManager::MountFS("/", MyROOT);
        Printk::Write(Printk::Level::LOG_INFO, "RootFS: Mounted RootFS at /\n");
    }
}

File *RootFS::Open(const char* path){
    if(path[0] == '/') path++; // skip leading slash

    if(path[0] == '\0'){
        File *froot = new File();
        froot->CurrentPosition = 0;
        froot->IsDirectory = TRUE; // root is a directory
        froot->FileSize = 0;
        String::Strcpy(froot->FileName, "/");
        froot->FSOwner = this;
        froot->RefCount = 1; // Init RefCount
        return froot;
    }

    return nullptr; // only root exists
}

RootFS::RootFS(){
    // Minimal constructor: nothing to initialize for simple RootFS
}

RootFS::~RootFS(){
    // Nothing to free for now
}

STATIC BOOL IsInHistory(char history[][32], int count, const char* name) {
    for(INTN i=0; i<count; i++) {
        if (String::Strcmp(history[i], name) == 0) return true;
    }
    return false;
}

INTN RootFS::ReadDir(File *dir, void *buf, U32 Size){
    if(!dir || !buf) return -1;

    CHAR8 FoundNames[32][32];
    INTN FoundCount = 0;

    char *out = (char*)buf;
    U32 remain = Size;
    U64 skip = dir->CurrentPosition;
    U64 seen = 0;
    
    // [FIX] Deklarasikan variabel ini di sini, JANGAN di tengah jalan
    U32 MPCount = VFSManager::GetMountPointCount();

    // 1. Output "." dan ".." sebagai Raw String
    CONSTANT CHAR8 *defaults[] = {".", ".."};
    for(INTN i = 0; i < 2; i++){
        U64 idx = seen++;
        if(idx < skip) continue;

        U32 NameLen = String::Strlen(defaults[i]);
        U32 Need = NameLen + 1; // +1 for Null Terminator

        // Sekarang aman pake goto karena MPCount sudah dideclare di atas
        if(Need > remain) { seen--; goto done;}
        
        // COPY STRING MENTAH
        String::Memcpy((U8*)out, (const U8*)defaults[i], Need);
        
        out += Need;
        remain -= Need;
    }

    // 2. Output Mount Points
    // MPCount sudah dideclare di atas, jadi tinggal loop
    for(U32 i = 0; i < MPCount; i++){
        CONSTANT CHAR8* MPPath = VFSManager::GetMountPointPath(i);
        
        // Validasi Path
        if(!MPPath || MPPath[0] != '/') continue; 
        if(MPPath[1] == '\0') continue; // Skip root sendiri

        CONSTANT CHAR8* Start = MPPath + 1; // Skip '/'

        // Parse nama folder pertama
        INTN Len = 0;
        while(Start[Len] != '/' && Start[Len] != '\0') Len++;
        if(Len >= 31) Len = 31;

        char FolderName[32];
        String::Memcpy((U8*)FolderName, (const U8*)Start, (U32)Len);
        FolderName[Len] = '\0';

        // Deduplikasi
        if(IsInHistory(FoundNames, FoundCount, FolderName)) continue; 
        String::Strcpy(FoundNames[FoundCount++], FolderName);

        U64 Idx = seen++;
        if(Idx < skip) continue;

        U32 NameLen = Len;
        U32 Need = NameLen + 1; // +1 for Null Terminator

        if(Need > remain) { seen--; break; } // Disini break oke karena gak loncat jauh

        // COPY STRING MENTAH
        String::Memcpy((U8*)out, (const U8*)FolderName, Need);

        out += Need;
        remain -= Need;
    }

done:
    dir->CurrentPosition = seen;
    return (INTN)(Size - remain);
}