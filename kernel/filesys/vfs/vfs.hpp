#pragma once
#include <rosval.h>
#include "../filesystem.hpp"
#include "../partition.hpp"

class FileSystem;
class Partition;
struct File;

namespace VFSManager{
    typedef FileSystem* (*FSDriverFactory)(); 

    VOID RegisterFileSystem(const char *fsName, FSDriverFactory factory);

    FileSystem* InstantiateDriver(const char *name);

    // Mount a partition at a virtual path (e.g. "/mnt/part0"). Path must be NUL-terminated.
    BOOL Mount(const char *path, Partition *Part);

    BOOL Mount(const char *path, const char *fsTypeName);

    // Resolve an absolute VFS path into (filesystem pointer, relative path inside FS)
    // Returns TRUE if a mounted FS matches a prefix of 'path'.
    BOOL ResolvePath(const char *path, FileSystem** outFS, char *OutRelativePath);

    // Convenience: mount partition to automatically chosen path? (Not implemented yet)
    BOOL Mount(Partition *Part);

    // File operations (paths are absolute in VFS space)
    File* Open(const char* path);
    File* Create(const char *Path);
    // Convenience: create parent directories as needed (mkdir -p behavior) then create file
    File* CreateWithParents(const char *Path);
    void Close(File* file);
    U32 Read(File* file, U8* buffer, U32 size); 
    U32 Write(File *File, U8 *Buffer, U32 Size);
    // Debug wrapper to trace write calls (see implementation in vfs.cpp)
    U32 DebugWrite(File *File, U8 *Buffer, U32 Size);
    U32 Append(const char* path, U8* Buffer, U32 Size);
    BOOL Delete(const char* path);
    BOOL Rename(const char* oldPath, const char* newPath);
    BOOL Seek(File* file, U64 position);
    BOOL Truncate(File* file, U64 size);
    BOOL MKDir(const char* path);
    BOOL RMDir(const char* path);
    BOOL Flush(File* file);
}