#pragma once
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

struct File {
    virtual ~File() {}
    
    // Info umum
    U64 FileSize;
    U64 InodeID = 0;
    U64 CurrentPosition;
    BOOL IsDirectory;
    char FileName[256]; // Nama file
    
    // Info internal driver
    U32 Internal_StartCluster;
    U32 Internal_CurrentCluster;
    U64 Internal_DirEntryLBA;     // LBA dari sektor yang berisi entri
    U32 Internal_DirEntryOffset;  // Offset byte di dalam sektor itu

    FileSystem *FSOwner;
    FileType type;
    
    // Reference Counting for Fork
    I32 RefCount;
    U32 Flags;

    virtual short Poll(short events) {
        return 0;
    }
};

class FileSystem{
    public:
        virtual ~FileSystem() {}

        virtual BOOL Mount(Partition *Part) = 0;
        virtual BOOL Unmount() = 0;
        virtual File* Open(const char* path, U32 Flags) = 0;
        virtual void Close(File* file) = 0;
        virtual U32 Read(File* file, U8* buffer, U32 size) = 0; 
        virtual U32 Write(File *File, U8 *Buffer, U32 Size) = 0;
        virtual BOOL UpdateDirectoryEntry(File* file){ return FALSE; } // default no-op
        virtual BOOL Delete(const char* path) = 0;
        virtual BOOL Rename(const char* oldPath, const char* newPath) = 0;
        virtual BOOL Seek(File* file, U64 position, U32 Origin) = 0; // allow seeking within regular files
        virtual BOOL Truncate(File* file, U64 size) = 0;
        virtual BOOL MKDir(const char* path) = 0;
        virtual BOOL RMDir(const char* path) = 0;
        virtual BOOL Flush(File* file) = 0;
        virtual BOOL Append(File* file, U8* buffer, U32 size) = 0;
        virtual INTN ReadDir(File* dirFile, void* buffer, U32 bufferSize) = 0;
        virtual INTN Ioctl(File* file, U32 command, U64 arg){ return -1; } // default no-op
        virtual BOOL Stat(const char* path, FileInfo* info) = 0;
        virtual I64 ReadLink(const char *path, char *outbuf, U64 MaxLen){ return -1; }
};