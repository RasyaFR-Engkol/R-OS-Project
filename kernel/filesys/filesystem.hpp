#pragma once
#include "spinlock/mutex.hpp"
#include <rosval.h>

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// Konstanta bitmask poll (Standar)
#define POLLIN      0x0001
#define POLLPRI     0x0002
#define POLLOUT     0x0004
#define POLLERR     0x0008
#define POLLHUP     0x0010
#define POLLNVAL    0x0020

class Partition;
class FileSystem;

enum FileType{
    FT_DIR = 0,
    FT_NORMAL,
    FT_PIPE,
    FT_SOCK,
    FT_DEVCHAR,
    FT_DEVBLOK,
    FT_SYMLINK,
    FT_SHM
};

struct FileInfo {
    U64 Size;
    U64 InodeID;
    FileType Type; // File, Directory, Device, dll
    U32 CreationTime; // Opsional nanti
    BOOL IsDirectory;
    U32 Mode;
};

struct Inode {
    U64 InodeID;
    U64 FileSize;
    FileType Type;
    FileSystem *FSOwner;
    
    // Info internal driver (FAT32, EXT2, dll)
    U32 Internal_StartCluster;
    U64 Internal_DirEntryLBA;     
    U32 Internal_DirEntryOffset;  
    
    // Sinkronisasi (Satpam)
    Mutex Lock; 
    I32 RefCount; // Berapa banyak struct File yang lagi nunjuk ke Inode ini

    Inode() {
        InodeID = 0;
        FileSize = 0;
        Type = FT_NORMAL;
        FSOwner = nullptr;
        Internal_StartCluster = 0;
        Internal_DirEntryLBA = 0;
        Internal_DirEntryOffset = 0;
        RefCount = 0;
    }
};

class FileSystem{
    public:
        virtual ~FileSystem() {}

        virtual BOOL Mount(Partition *Part) = 0;
        virtual BOOL Unmount() = 0;
        // GANTI JADI 3 FUNGSI INI (Wajib di-implementasi sama EXT2, DevFS, dll):
    
        // 1. Cuma nyari path ini InodeID-nya berapa? (Return 0 kalo gak ketemu)
        virtual U64 Lookup(const char* path) = 0;

        // 2. Baca spek Inode dari disk (ukuran, tipe), terus masukin ke VFSNode
        virtual BOOL PopulateInode(U64 InodeID, ::Inode* vfsNode) = 0;

        // 3. Khusus buat bikin file baru (O_CREAT), return InodeID yang baru dibikin
        virtual U64 CreateNode(const char* path, U32 Flags) = 0;
        
        virtual void Close(File* file) = 0;
        virtual U32 Read(File* file, U8* buffer, U32 size) = 0; 
        virtual U32 Write(File *File, U8 *Buffer, U32 Size) = 0;
        virtual BOOL UpdateDirectoryEntry(File* file){ return FALSE; } 
        virtual BOOL Delete(const char* path) = 0;
        virtual BOOL Rename(const char* oldPath, const char* newPath) = 0;
        virtual BOOL Seek(File* file, U64 position, U32 Origin) = 0; 
        virtual BOOL Truncate(File* file, U64 size) = 0;
        virtual BOOL MKDir(const char* path) = 0;
        virtual BOOL RMDir(const char* path) = 0;
        virtual BOOL Flush(File* file) = 0;
        virtual BOOL Append(File* file, U8* buffer, U32 size) = 0;
        virtual INTN ReadDir(File* dirFile, void* buffer, U32 bufferSize) = 0;
        virtual INTN Ioctl(File* file, U32 command, U64 arg){ return -1; } 
        virtual BOOL Stat(const char* path, FileInfo* info) = 0;
        virtual I64 ReadLink(const char *path, char *outbuf, U64 MaxLen){ return -1; }
        virtual short Poll(File* file, short events) { return 0; } // Default return 0
};

struct File {
    virtual ~File() {}
    
    U64 CurrentPosition; // Offset baca/tulis khusus sesi ini
    U32 Flags;
    char FileName[256];
    BOOL IsDirectory;
    
    // Pointer sakti penghubung sesi ke file fisik!
    Inode* Node; 
    
    I32 RefCount; // RefCount buat copy File Descriptor (misal pas fork)
    FileType type;

    VOID *PrivateData;
    
    virtual short Poll(short events) {
        if (Node && Node->FSOwner) {
            return Node->FSOwner->Poll(this, events);
        }
        return 0;
    }
};