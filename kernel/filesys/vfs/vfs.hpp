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
        FileSize = 0;
        CurrentPosition = 0;
        // FSOwner nanti di-set dari luar
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

void RemoveNamedPipeEntry(PipeBuffer* targetBuf);

// --- DRIVER (Jembatan ke VFS) ---
class PipeFileSystem : public FileSystem {
private:
    CONSTANT U32 BUFFERSIZE = 4096;
public:
    // Singleton Instance (biar hemat memori, cukup 1 driver buat semua pipe)
    static PipeFileSystem* GetInstance();

    // Implementasi Virtual Function dari FileSystem
    
    BOOL Mount(Partition *Part) override { return TRUE; }
    BOOL Unmount() override {return false;}
    File* Open(const char* path, U32 Flags) override { return nullptr; } // Pipe gak bisa di-open via path

    // LOGIC BACA (Pindahan dari kode kamu tadi)
    U32 Read(File* file, U8* Buffer, U32 Size) override {
        PipeFile *PF = (PipeFile*)file;
        if(PF->IsWriteEnd) return 0; // pembacaan masih mungkin kalo caller batu

        U32 RDCount = 0;
        PipeBuffer *buf = PF->buf;
        buf->Lock.Acquire();

        while(RDCount < Size){
            if(buf->BytesAvailable == 0){
                if(buf->IsWriteClosed){
                    break;
                }
                break;
            }
            U32 BytesNeeded = Size - RDCount;
            U32 BytesAvailable = buf->BytesAvailable;
            U32 BytesToEnd = BUFFERSIZE - buf->ReadPos;

            // Ambil minimum: Butuh berapa vs Ada berapa vs Mentok ujung array
            U32 ChunkSize = String::Min(BytesNeeded, BytesAvailable);
            ChunkSize = String::Min(ChunkSize, BytesToEnd);

            // --- 3. MEMCPY ---
            String::Memcpy(&Buffer[RDCount], &buf->data[buf->ReadPos], ChunkSize);

            // --- 4. UPDATE POINTERS ---
            buf->ReadPos = (buf->ReadPos + ChunkSize) % BUFFERSIZE;
            buf->BytesAvailable -= ChunkSize;
            RDCount += ChunkSize;

            // Bangunin Writer! (Woy udah agak kosong nih, silakan tulis lagi!)
            Tasking::WakeUp(buf->FullWaiters);
        }
        buf->Lock.Release();
        return RDCount;
    }

    // LOGIC TULIS
    U32 Write(File *file, U8 *Buf, U32 Size) override {
        PipeFile* pfile = (PipeFile*)file;
        if(!pfile->IsWriteEnd) return 0;

        U32 WriteCount = 0;
        PipeBuffer* buf = pfile->buf;
        buf->Lock.Acquire();

        while(WriteCount < Size){

            while(buf->BytesAvailable == BUFFERSIZE){
                if(buf->IsWriteClosed){
                    buf->Lock.Release();
                    return WriteCount;
                }

                buf->Lock.Release();
                Tasking::SleepOn(buf->FullWaiters);
                buf->Lock.Acquire();
            }

            U32 BytesToTransfer = Size - WriteCount;
            U32 SpaceAvail = BUFFERSIZE - buf->BytesAvailable;
            U32 BytesToEnd = BUFFERSIZE - buf->WritePos;

            U32 ChunkSize = String::Min(BytesToTransfer, SpaceAvail);
            ChunkSize = String::Min(ChunkSize, BytesToEnd);
            
            String::Memcpy(&buf->data[buf->WritePos], &Buf[WriteCount], ChunkSize);

            buf->WritePos = (buf->WritePos + ChunkSize) % BUFFERSIZE;
            buf->BytesAvailable += ChunkSize;
            WriteCount += ChunkSize;

            Tasking::WakeUp(buf->Waiters);
        }
        buf->Lock.Release();
        return WriteCount;
    }

    void Close(File* file) override {
        PipeFile* pfile = (PipeFile*)file;
        PipeBuffer* buf = pfile->buf; // Ambil pointer buffer

        if(!buf) { delete pfile; return; }

        buf->Lock.Acquire();

        // 1. Logic EOF Writer (Yang lama tetep ada)
        if(pfile->IsWriteEnd) {
            buf->IsWriteClosed = TRUE;
            Tasking::WakeUp(buf->Waiters);
            Tasking::WakeUp(buf->FullWaiters);
        }
        else {
            Tasking::WakeUp(buf->FullWaiters);
        }

        // 2. [BARU] Logic Memory Management
        buf->RefCount--; // Kurangi 1 nyawa

        BOOL shouldDelete = (buf->RefCount == 0);

        buf->Lock.Release();

        if (shouldDelete) {
            RemoveNamedPipeEntry(buf);
            delete buf; 
            // Printk::Write(Printk::Level::LOG_INFO, "Pipe resource destroyed.\n");
        }

        delete pfile;
    }

    // Dummy overrides
    BOOL Delete(const char* path) override { return FALSE; }
    BOOL Rename(const char* o, const char* n) override { return FALSE; }
    BOOL Seek(File* f, U64 p, U32 o) override { return FALSE; }
    BOOL Truncate(File* f, U64 s) override { return FALSE; }
    BOOL MKDir(const char* path) override { return FALSE; }
    BOOL RMDir(const char* path) override { return FALSE; }
    BOOL Flush(File* file) override { return TRUE; }
    BOOL Append(File* file, U8* buffer, U32 size) override { return FALSE; }
    BOOL Stat(const char* path, FileInfo* info) override {return false;}
    INTN ReadDir(File* dirFile, void* buffer, U32 bufferSize) override {
        return -1; // Error: Pipe bukan direktori, tidak bisa di-ls
    }
};

// Global instance without using function-local static to avoid __cxa_guard issues
// Use a pointer initialized at runtime to avoid any static object initialization
inline PipeFileSystem* g_PipeFileSystemInstance = nullptr;

inline PipeFileSystem* PipeFileSystem::GetInstance(){
    if(!g_PipeFileSystemInstance){
        g_PipeFileSystemInstance = new PipeFileSystem();
    }
    return g_PipeFileSystemInstance;
}