#pragma once
#include <rosval.h>
#include "../filesystem.hpp"
#include "../partition.hpp"
// Forward-declare SchedulerYield to avoid include-order circularities
namespace Tasking { void SchedulerYield(); }
#include <task.hpp>

class FileSystem;
class Partition;
struct File;

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
    INTN ReadDir(File* dirFile, void* buffer, U32 bufferSize);
    INTN Ioctl(File* file, U32 command, U64 arg);
}

// pipa
struct PipeBuffer{
    CHAR8 data[4096];
    INTN ReadPos;
    INTN WritePos;
    INTN BytesAvailable;
    BOOL IsWriteClosed;
    INTN RefCount;
};

// PipeFile Tetap inherit File, tapi fungsinya dipindah
struct PipeFile : public File {
    PipeBuffer* buf;
    BOOL IsWriteEnd;

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
};

// --- DRIVER (Jembatan ke VFS) ---
class PipeFileSystem : public FileSystem {
public:
    // Singleton Instance (biar hemat memori, cukup 1 driver buat semua pipe)
    static PipeFileSystem* GetInstance();

    // Implementasi Virtual Function dari FileSystem
    
    BOOL Mount(Partition *Part) override { return TRUE; }
    File* Open(const char* path) override { return nullptr; } // Pipe gak bisa di-open via path
    File* Create(const char *Path) override { return nullptr; }

    // LOGIC BACA (Pindahan dari kode kamu tadi)
    U32 Read(File* file, U8* Buffer, U32 Size) override {
        // Casting balik ke PipeFile
        PipeFile* pfile = (PipeFile*)file;

        if(pfile->IsWriteEnd) return 0; 

        U32 RDCount = 0;
        PipeBuffer* buf = pfile->buf;

        while(RDCount < Size){
            if(buf->BytesAvailable == 0){
                if (buf->IsWriteClosed) break; // EOF
                
                Tasking::SchedulerYield();
                continue; // Buffer kosong, tunggu data masuk
            }
            Buffer[RDCount++] = buf->data[buf->ReadPos];
            buf->ReadPos = (buf->ReadPos + 1) % 4096;
            buf->BytesAvailable--;
        }
        return RDCount;
    }

    // LOGIC TULIS
    U32 Write(File *file, U8 *Buf, U32 Size) override {
        PipeFile* pfile = (PipeFile*)file;
        
        if(!pfile->IsWriteEnd) return 0;

        U32 WriteCount = 0;
        PipeBuffer* buf = pfile->buf;

        while(WriteCount < Size){
            if(buf->BytesAvailable >= 4096){
                break; // Buffer full
            }
            buf->data[buf->WritePos] = Buf[WriteCount++];
            buf->WritePos = (buf->WritePos + 1) % 4096;
            buf->BytesAvailable++;
        }
        return WriteCount;
    }

    void Close(File* file) override {
        PipeFile* pfile = (PipeFile*)file;
        PipeBuffer* buf = pfile->buf; // Ambil pointer buffer

        if(!buf) return; // Safety check

        // 1. Logic EOF Writer (Yang lama tetep ada)
        if(pfile->IsWriteEnd) {
            buf->IsWriteClosed = TRUE;
        }

        // 2. [BARU] Logic Memory Management
        buf->RefCount--; // Kurangi 1 nyawa

        if (buf->RefCount == 0) {
            // Kalau udah gak ada yang pake (0), berarti Reader & Writer udah close semua.
            // Saatnya bersihkan dari RAM!
            delete buf; 
            
            // Opsional: Debugging
            // Printk::Write(Printk::Level::LOG_INFO, "PipeBuffer freed.\n");
        }
    }

    // Dummy overrides
    BOOL Delete(const char* path) override { return FALSE; }
    BOOL Rename(const char* o, const char* n) override { return FALSE; }
    BOOL Seek(File* f, U64 p) override { return FALSE; }
    BOOL Truncate(File* f, U64 s) override { return FALSE; }
    BOOL MKDir(const char* path) override { return FALSE; }
    BOOL RMDir(const char* path) override { return FALSE; }
    BOOL Flush(File* file) override { return TRUE; }
    BOOL Append(File* file, U8* buffer, U32 size) override { return FALSE; }
    BOOL Cp(const char* srcPath, const char* destPath) override { return FALSE; }
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