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

void RemoveNamedPipeEntry(PipeBuffer* targetBuf);
PipeBuffer *GetNamedPipeBuffer(const CHAR8* name);
PipeBuffer *CreateNamedPipe(const CHAR8* Name);

// --- DRIVER (Jembatan ke VFS) ---
class PipeFileSystem : public FileSystem {
private:
    CONSTANT U32 BUFFERSIZE = 4096;
public:
    static PipeFileSystem* GetInstance();

    BOOL Mount(Partition *Part) override { return TRUE; }
    BOOL Unmount() override { return FALSE; }

    // ==============================================================
    // [BARU] 3 FUNGSI KULI VFS
    // ==============================================================
    U64 Lookup(const char* path) override {
        if (!path || path[0] == '\0') return 0;
        // Cari named pipe dari helper lu
        PipeBuffer* pipe = GetNamedPipeBuffer(path);
        if (pipe) return (UPTR)pipe; // Alamat memori jadi InodeID
        return 0;
    }

    BOOL PopulateInode(U64 InodeID, ::Inode* vfsNode) override {
        if (InodeID == 0 || !vfsNode) return FALSE;
        PipeBuffer* pipe = (PipeBuffer*)(UPTR)InodeID;

        vfsNode->Type = FT_PIPE;
        vfsNode->FSOwner = this;
        vfsNode->FileSize = pipe->BytesAvailable; 
        vfsNode->InodeID = InodeID;
        return TRUE;
    }

    U64 CreateNode(const char* path, U32 Flags) override {
        if (!path || path[0] == '\0') {
            Printk::Write(Printk::Level::LOG_ERR, "PipeFS: CreateNode called with invalid path\n");
            return 0;
        }
        PipeBuffer* pipe = CreateNamedPipe(path);
        if (!pipe) {
            Printk::Write(Printk::Level::LOG_ERR, "PipeFS: Failed to create named pipe\n");
            return 0;
        }
        return (UPTR)pipe; // Alamat memori pipe baru jadi InodeID
    }
    // ==============================================================

    // LOGIC BACA
    U32 Read(File* file, U8* Buffer, U32 Size) override {
        if(!file || !file->Node || !Buffer || Size == 0) return 0;

        PipeBuffer *buf = (PipeBuffer*)(UPTR)file->Node->InodeID;
        if(!buf) return 0;

        if((file->Flags & O_WRONLY) && !(file->Flags & O_RDWR)) return 0;

        U32 RDCount = 0;
        buf->Lock.Acquire();

        while(RDCount < Size){
            // --- BAGIAN YANG DIREVISI ---
            while(buf->BytesAvailable == 0){
                if(buf->IsWriteClosed){
                    // Kalau writer udah tutup pipanya, kita berhenti baca.
                    buf->Lock.Release();
                    return RDCount;
                } 

                if(file->Flags & O_NONBLOCK) {
                    buf->Lock.Release();
                    return RDCount; // Return 0 (atau jumlah bytes yg udh sempet kebaca)
                }

                buf->Lock.Release();
                Tasking::SleepOn(buf->Waiters); // Tidur nunggu ada yang nulis
                buf->Lock.Acquire();            // Bangun-bangun, ambil lock lagi
            }
            // -----------------------------

            U32 BytesNeeded = Size - RDCount;
            U32 BytesAvailable = buf->BytesAvailable;
            U32 BytesToEnd = BUFFERSIZE - buf->ReadPos;

            U32 ChunkSize = String::Min(BytesNeeded, BytesAvailable);
            ChunkSize = String::Min(ChunkSize, BytesToEnd);

            String::Memcpy(&Buffer[RDCount], &buf->data[buf->ReadPos], ChunkSize);

            buf->ReadPos = (buf->ReadPos + ChunkSize) % BUFFERSIZE;
            buf->BytesAvailable -= ChunkSize;
            RDCount += ChunkSize;

            // Bangunin si Writer yang mungkin ketiduran gara-gara pipa kepenuhan
            Tasking::WakeUp(buf->FullWaiters);
        }
        buf->Lock.Release();
        return RDCount;
    }

    // LOGIC TULIS
    U32 Write(File *file, U8 *Buf, U32 Size) override {
        if(!file || !file->Node || !Buf || Size == 0) return 0;

        PipeBuffer* buf = (PipeBuffer*)(UPTR)file->Node->InodeID;
        if(!buf) return 0;

        // Kalau file ini dibuka cuma buat BACA, ngapain dia nulis?
        // (O_RDONLY biasanya 0, O_WRONLY biasanya 1, O_RDWR biasanya 2)
        if(!(file->Flags & O_WRONLY) && !(file->Flags & O_RDWR)) return 0;

        U32 WriteCount = 0;
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

    // LOGIC CLOSE (RAM Friendly!)
    void Close(File* file) override {
        if(!file || !file->Node) return;

        PipeBuffer* buf = (PipeBuffer*)(UPTR)file->Node->InodeID;
        if(!buf) return;

        buf->Lock.Acquire();

        // 1. Logic EOF Writer: Cek apakah yang nge-close ini Writer?
        if((file->Flags & O_WRONLY) || (file->Flags & O_RDWR)) {
            buf->IsWriteClosed = TRUE;
            Tasking::WakeUp(buf->Waiters);
            Tasking::WakeUp(buf->FullWaiters);
        } else {
            Tasking::WakeUp(buf->FullWaiters);
        }

        // 2. Logic Memory Management
        if(buf->RefCount > 0) buf->RefCount--; 
        BOOL shouldDelete = (buf->RefCount == 0);

        buf->Lock.Release();

        if (shouldDelete) {
            RemoveNamedPipeEntry(buf);
            delete buf; 
        }

        // PENTING: Gak ada lagi 'delete pfile;' di sini! Biar VFS yang ngurus!
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
    BOOL Stat(const char* path, FileInfo* info) override {
        // Biar konsisten sama Lookup
        U64 id = Lookup(path);
        if(id == 0) return FALSE;
        PipeBuffer* buf = (PipeBuffer*)(UPTR)id;
        info->Size = buf->BytesAvailable;
        info->Type = FT_PIPE;
        info->IsDirectory = FALSE;
        info->InodeID = id;
        return TRUE;
    }
    INTN ReadDir(File* dirFile, void* buffer, U32 bufferSize) override { return -1; }

    short Poll(File* file, short events) override {
        if(!file || !file->Node) return 0;

        PipeBuffer* buf = (PipeBuffer*)(UPTR)file->Node->InodeID;
        if(!buf) return 0;

        short revents = 0;
        buf->Lock.Acquire();

        if (events & POLLIN) {
            // Ada data buat dibaca ATAU writer udah tutup pipanya (EOF)
            if (buf->BytesAvailable > 0 || buf->IsWriteClosed) {
                revents |= POLLIN;
            }
        }

        if (events & POLLOUT) {
            // Masih ada sisa ruang buat nulis
            if (buf->BytesAvailable < BUFFERSIZE) {
                revents |= POLLOUT;
            }
        }

        buf->Lock.Release();
        return revents;
    }
};

// Global instance 
inline PipeFileSystem* g_PipeFileSystemInstance = nullptr;

inline PipeFileSystem* PipeFileSystem::GetInstance(){
    if(!g_PipeFileSystemInstance){
        g_PipeFileSystemInstance = new PipeFileSystem();
    }
    return g_PipeFileSystemInstance;
}