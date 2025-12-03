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
    RootFS *g_GlobalRootFS = nullptr;

    VOID InitROOTFS(){
        // [FIX] Assign langsung ke variabel GLOBAL, jangan bikin variabel lokal baru
        g_GlobalRootFS = new RootFS(); 
        
        // Mount pakai variabel global
        VFSManager::MountFS("/", g_GlobalRootFS);
        
        Printk::Write(Printk::Level::LOG_INFO, "RootFS: Mounted RootFS at /\n");
    }

    RootFS *GetRootFS(){
        return g_GlobalRootFS;
    }
}

RootFS::RootFS(){
    m_BackendFS = nullptr;
    m_BackendRootHandle = nullptr;
}

VOID RootFS::SetBackingFileSystem(FileSystem *fs){
    this->m_BackendFS = fs;
    if(fs){
        this->m_BackendRootHandle = fs->Open("/");
        Printk::Write(Printk::Level::LOG_INFO, "RootFS: Set backing filesystem successfully.\n");
    }
}

File *RootFS::Open(const char* path){
    // [FIX] Handle variasi input path: kosong ("") atau slash ("/")
    // Dua-duanya harus dianggap sebagai request buka ROOT directory virtual
    if(path[0] == '\0' || (path[0] == '/' && path[1] == '\0')){
        
        File *froot = new File();
        
        // Setup Virtual File Handle
        froot->CurrentPosition = 0;
        froot->IsDirectory = TRUE;
        froot->FileSize = 0;
        String::Strcpy(froot->FileName, "/");
        
        // [PENTING] Ownernya harus THIS (RootFS), biar ReadDir manggil RootFS::ReadDir
        froot->FSOwner = this; 
        froot->RefCount = 1;
        
        // Printk::Write(Printk::Level::LOG_DEBUG, "RootFS: Opened Virtual Root Handle\n");
        return froot;
    }

    // Kalau bukan root (misal /etc/config), baru oper ke EXT2
    if(m_BackendFS){
        // Printk::Write(Printk::Level::LOG_DEBUG, "RootFS: Forwarding '%s' to Backend\n", path);
        return m_BackendFS->Open(path);
    }

    return nullptr; 
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

STATIC BOOL EmitEntry(char* name, char** outBuffer, U32* remainSize, U64* currentFilePos, U64 virtualOffset) {
    U32 nameLen = String::Strlen(name);
    U32 need = nameLen + 1; // +1 buat Null Terminator

    // Cek apakah posisi file cursor user ada di dalam range item ini
    // Logika: Apakah byte awal item ini >= posisi cursor user?
    if (virtualOffset >= *currentFilePos) {
        // Cek buffer muat gak
        if (need > *remainSize) return TRUE; // Buffer penuh, stop

        // Copy string
        String::Memcpy((U8*)*outBuffer, (const U8*)name, need);
        
        // Majuin pointer buffer output
        *outBuffer += need;
        *remainSize -= need;
        
        // Update posisi file user (INI KUNCINYA)
        *currentFilePos += need; 
    }
    
    return FALSE;
}

INTN RootFS::ReadDir(File *dir, void *buf, U32 Size){
    //Printk::Write(Printk::Level::LOG_DEBUG, "RootFS: ReadDir called. Size=%u, CurrentPos=%llu\n", Size, (unsigned long long)dir->CurrentPosition);
    if(!dir || !buf) return -1;

    char *out = (char*)buf;
    U32 remain = Size;
    
    U64 targetIndex = dir->CurrentPosition; 
    U64 currentIndex = 0; 

        // --- 2. Mount Points (DEBUGGED) ---
    CHAR8 FoundNames[16][32];
    INTN FoundCount = 0;
    U32 MPCount = VFSManager::GetMountPointCount();

    // --- 1. Static Items (. dan ..) ---
    CONSTANT CHAR8 *defaults[] = {".", ".."};
    
    for(INTN i = 0; i < 2; i++){
        if (currentIndex >= targetIndex) {
            U32 len = String::Strlen(defaults[i]) + 1;
            if (remain < len) goto done; 

            String::Memcpy((U8*)out, (const U8*)defaults[i], len);
            out += len;
            remain -= len;
            dir->CurrentPosition++; 
        }
        currentIndex++;
    }

    // [DEBUG] Cek jumlah Mount Point yang terdeteksi
    // Printk::Write(Printk::Level::LOG_DEBUG, "RootFS: Scanning MountPoints. Total MPCount=%d\n", MPCount);

    for(U32 i = 0; i < MPCount; i++){
        CONSTANT CHAR8* MPPath = VFSManager::GetMountPointPath(i);
        
        // [DEBUG] Cek path mentah
        //Printk::Write(Printk::Level::LOG_DEBUG, "  MP[%d] RawPath='%s'\n", i, MPPath ? MPPath : "NULL");

        if(!MPPath || MPPath[0] != '/') continue; 
        if(MPPath[1] == '\0') continue; // Skip root

        CONSTANT CHAR8* Start = MPPath + 1; 
        INTN Len = 0;
        // Parse nama folder pertama (misal 'mnt' dari '/mnt/data')
        while(Start[Len] != '/' && Start[Len] != '\0') Len++;
        
        if(Len >= 31) Len = 31;

        char FolderName[32];
        String::Memcpy((U8*)FolderName, (const U8*)Start, (U32)Len);
        FolderName[Len] = '\0';

        // [DEBUG] Cek hasil parsing nama folder
        //Printk::Write(Printk::Level::LOG_DEBUG, "    -> Parsed FolderName='%s'\n", FolderName);

        if(IsInHistory(FoundNames, FoundCount, FolderName)) {
            // [DEBUG] Kena filter deduplikasi
            //Printk::Write(Printk::Level::LOG_DEBUG, "    -> Skipped (Duplicate)\n");
            continue; 
        }
        String::Strcpy(FoundNames[FoundCount++], FolderName);

        // --- Logic Copy ---
        if (currentIndex >= targetIndex) {
            U32 itemLen = String::Strlen(FolderName) + 1;
            if (remain < itemLen) goto done;
            
            String::Memcpy((U8*)out, (const U8*)FolderName, itemLen);
            out += itemLen;
            remain -= itemLen;
            dir->CurrentPosition++;
            
            // [DEBUG] Berhasil ditulis ke buffer user
            //Printk::Write(Printk::Level::LOG_DEBUG, "    -> EMITTED to user buffer!\n");
        }
        currentIndex++;
    }

    // --- 3. Backend Filesystem (EXT2) ---
    if (m_BackendFS && m_BackendRootHandle) {
        
        if (dir->CurrentPosition >= currentIndex) {
            
            // Rumus Index: (UserPos - VirtualItems) + 2 (skip . dan .. EXT2)
            U64 ext2RequestIndex = (dir->CurrentPosition - currentIndex) + 2;
            
            // [DEBUG] Transisi ke EXT2
            //Printk::Write(Printk::Level::LOG_DEBUG, "RootFS: Switching to EXT2. UserPos=%d VirtualItems=%d Ext2Req=%d\n", 
            //    (int)dir->CurrentPosition, (int)currentIndex, (int)ext2RequestIndex);

            m_BackendRootHandle->CurrentPosition = ext2RequestIndex;
            
            char* ext2Buf = (char*)Kmalloc::Alloc(remain);
            if (ext2Buf) {
                INTN bytesRead = m_BackendFS->ReadDir(m_BackendRootHandle, ext2Buf, remain);
                
                if (bytesRead > 0) {
                    // Copy RAW bytes dulu
                    String::Memcpy((U8*)out, (const U8*)ext2Buf, bytesRead);
                    
                    // Hitung jumlah item string yang barusan dicopy buat update index
                    U32 processed = 0;
                    U32 itemsCount = 0;
                    while(processed < (U32)bytesRead){
                        char* s = ext2Buf + processed;
                        U32 sLen = String::Strlen(s) + 1;
                        processed += sLen;
                        itemsCount++;
                        // [DEBUG] Item dari EXT2
                        // Printk::Write(Printk::Level::LOG_DEBUG, "  EXT2 Item: %s\n", s);
                    }
                    
                    out += bytesRead;
                    remain -= bytesRead;
                    dir->CurrentPosition += itemsCount;
                }
                Kmalloc::Free(ext2Buf);
            }
        }
    }

done:
    return (INTN)(Size - remain);
}