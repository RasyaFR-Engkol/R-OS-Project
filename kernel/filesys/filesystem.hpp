#pragma once
#include <rosval.h>

class Partition;
class FileSystem;

struct File {
    virtual ~File() {}
    
    // Info umum
    U64 FileSize;
    U64 CurrentPosition;
    BOOL IsDirectory;
    char FileName[256]; // Nama file
    
    // Info internal driver
    U32 Internal_StartCluster;
    U32 Internal_CurrentCluster;
    U64 Internal_DirEntryLBA;     // LBA dari sektor yang berisi entri
    U32 Internal_DirEntryOffset;  // Offset byte di dalam sektor itu

    FileSystem *FSOwner;
};

class FileSystem{
    public:
        virtual ~FileSystem() {}

        virtual BOOL Mount(Partition *Part) = 0;
        virtual File* Open(const char* path) = 0;
        virtual File* Create(const char *Path) = 0;
        virtual void Close(File* file) = 0;
        virtual U32 Read(File* file, U8* buffer, U32 size) = 0; 
        virtual U32 Write(File *File, U8 *Buffer, U32 Size) = 0;
        virtual BOOL UpdateDirectoryEntry(File* file){ return FALSE; } // default no-op
        virtual BOOL Delete(const char* path) = 0;
        virtual BOOL Rename(const char* oldPath, const char* newPath) = 0;
        virtual BOOL Seek(File* file, U64 position) = 0; // allow seeking within regular files
        virtual BOOL Truncate(File* file, U64 size) = 0;
        virtual BOOL MKDir(const char* path) = 0;
        virtual BOOL RMDir(const char* path) = 0;
        virtual BOOL Flush(File* file) = 0;
        virtual BOOL Append(File* file, U8* buffer, U32 size) = 0;
        virtual BOOL Cp(const char* srcPath, const char* destPath) = 0;
};