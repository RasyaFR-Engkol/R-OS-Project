#pragma once
#include <rosval.h>
#include "../filesystem.hpp"
#include "../partition.hpp"
#include "spinlock/simple.hpp"
// Forward-declare SchedulerYield to avoid include-order circularities
namespace Tasking { void SchedulerYield(); }
#include <task.hpp>

class FileSystem;
class Partition;
struct File;

#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040  // Create file if it doesn't exist
#define O_EXCL      0x0080  // Error if O_CREAT and file exists
#define O_TRUNC     0x0200  // Truncate file to 0 length
#define O_APPEND    0x0400  // Append mode
#define O_NONBLOCK  0x00000800  // Atau 04000 dalam octal

#define MAX_SYMLINK_DEPTH 16

namespace VFSManager{
    typedef FileSystem* (*FSDriverFactory)(); 

    VOID RegisterFileSystem(const char *fsName, FSDriverFactory factory);

    FileSystem* InstantiateDriver(const char *name);

    U32 GetMountPointCount();
    CONSTANT CHAR8* GetMountPointPath(U32 index);

    // Mount a partition at a virtual path (e.g. "/mnt/part0"). Path must be NUL-terminated.
    BOOL Mount(const char *path, Partition *Part);

    BOOL Mount(const char *path, const char *fsTypeName);
    
    // Mount an already-instantiated filesystem object at a path (useful for
    // pseudo-filesystems like DevFS which are not backed by a Partition)
    BOOL MountFS(const char *path, FileSystem* fs);

    // Resolve an absolute VFS path into (filesystem pointer, relative path inside FS)
    // Returns TRUE if a mounted FS matches a prefix of 'path'.
    BOOL ResolvePath(const char *path, FileSystem** outFS, char *OutRelativePath, BOOL FollowLastSymlink = FALSE);
    BOOL FindMountPoint(const char *path, FileSystem** outFS, char *OutRelativePath);

    // Convenience: mount partition to automatically chosen path? (Not implemented yet)
    BOOL Mount(Partition *Part);

    // File operations (paths are absolute in VFS space)
    File* Open(const char* path, U32 Flags);
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
    BOOL Seek(File* file, U64 position, U32 origin);
    BOOL Truncate(File* file, U64 size);
    BOOL MKDir(const char* path);
    BOOL RMDir(const char* path);
    BOOL Flush(File* file);
    INTN ReadDir(File* dirFile, void* buffer, U32 bufferSize);
    INTN Ioctl(File* file, U32 command, U64 arg);
    BOOL SyncAll();
    BOOL MountDevice(const char* devicePath, const char* mountPath);
}

// pipa
struct PipeBuffer{
    CHAR8 data[4096];
    INTN ReadPos;
    INTN WritePos;
    U32 BytesAvailable;
    BOOL IsWriteClosed;
    INTN RefCount;
    Arch::Spinlock::Spinlock Lock;

    Tasking::WaitQueue Waiters;
    Tasking::WaitQueue FullWaiters;
};

// PipeFile Tetap inherit File, tapi fungsinya dipindah
struct PipeFile : public File {
    PipeBuffer* buf;
    BOOL IsWriteEnd;
    CONSTANT U32 BufferSize = 4096;

    // Konstruktor
    PipeFile(PipeBuffer *Buf, BOOL WriteMode){
        buf = Buf;
        IsWriteEnd = WriteMode;
        RefCount = 1;
        IsDirectory = FALSE;
        CurrentPosition = 0;
        type = FT_PIPE;
        
        // [PENTING] Pipe jalan di memori, gak punya file fisik di disk.
        // Jadi Node kita set nullptr. Driver PipeFileSystem lu bakal
        // ngelewatin ini dan pake buf->Lock buat sinkronisasi.
        Node = nullptr; 
    }

    virtual short Poll(short events) override {
        short revents = 0;
        if(!buf) return POLLERR;

        // WAJIB LOCK! Karena BytesAvailable bisa berubah dari CPU core lain
        buf->Lock.Acquire();

        // 1. LOGIC BUAT READER (Mau baca data)
        if (events & POLLIN) {
            // Kita cuma peduli kalau kita adalah Reader (atau mode R/W)
            if (!IsWriteEnd) {
                // Ada data? ATAU Writer udah tutup (EOF)?
                // Kalau Writer tutup, Poll tetap return POLLIN biar Read() jalan & return 0 (EOF)
                if (buf->BytesAvailable > 0 || buf->IsWriteClosed) {
                    revents |= POLLIN;
                }
            }
        }

        // 2. LOGIC BUAT WRITER (Mau kirim data)
        if (events & POLLOUT) {
            // Kita cuma peduli kalau kita Writer
            if (IsWriteEnd) {
                // Buffer masih muat gak?
                if (buf->BytesAvailable < 4096) {
                    revents |= POLLOUT;
                }
            }
        }

        // 3. ERROR HANDLING (Opsional)
        // Kalau Reader mau baca tapi Writer gak ada (Pipe putus), 
        // biasanya di UNIX return POLLHUP, tapi POLLIN + EOF di Read() juga cukup.
        if (buf->IsWriteClosed && !IsWriteEnd) {
            revents |= POLLHUP; 
        }

        buf->Lock.Release();
        return revents;
    }
};