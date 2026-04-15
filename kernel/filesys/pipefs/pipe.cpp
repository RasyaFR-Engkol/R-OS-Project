#include <rosval.h>
#include "pipe.hpp"

PipeFileSystem* g_PipeFileSystemInstance = nullptr;

#define MAXIMAL_PIPE_WITHOUT_MEMORY 50

struct NamedPipeEntry{
    CHAR8 Name[64];
    PipeBuffer *Buffer;
};
NamedPipeEntry NamedPipe[50]; // limit 10 aja dulu

void RemoveNamedPipeEntry(PipeBuffer* targetBuf) {
    for(INTN i = 0; i < MAXIMAL_PIPE_WITHOUT_MEMORY; i++){
        if(NamedPipe[i].Buffer == targetBuf){
            // Reset entry biar bisa dipake lagi & ga dangling pointer
            NamedPipe[i].Buffer = nullptr;
            NamedPipe[i].Name[0] = '\0';
            return;
        }
    }
}

PipeBuffer *GetNamedPipeBuffer(const CHAR8* name){
    for(INTN i = 0; i < MAXIMAL_PIPE_WITHOUT_MEMORY;i++){
        if(String::Strcmp(NamedPipe[i].Name, name) == 0){
            return NamedPipe[i].Buffer;
        }
    }
    return nullptr;
}

PipeBuffer *CreateNamedPipe(const CHAR8* Name){
    PipeBuffer *buf = new PipeBuffer();
    buf->RefCount = 0;

    for(INTN i = 0; i < MAXIMAL_PIPE_WITHOUT_MEMORY; i++){
        if(!NamedPipe[i].Buffer){
            String::Strcpy(NamedPipe[i].Name, Name);
            buf->BytesAvailable = 0;
            buf->IsWriteClosed = FALSE;
            buf->WritePos = 0;
            buf->ReadPos = 0;
            buf->RefCount = 0;
            buf->Lock.Init();
            NamedPipe[i].Buffer = buf;
            Printk::Write(Printk::Level::LOG_INFO, "DevFS: Created named pipe '%s'\n", Name);
            return buf;
        }
    }
    Printk::Write(Printk::Level::LOG_ERR, "DevFS: Failed to create named pipe '%s', limit reached\n", Name);
    return nullptr; 
}

PipeFileSystem* PipeFileSystem::GetInstance(){
    if(!g_PipeFileSystemInstance) return nullptr; 
    // Make sure PipeFileSystem initialized in first place
    return g_PipeFileSystemInstance;
}

U64 PipeFileSystem::Lookup(const char* path){
    if (!path || path[0] == '\0') return 0;
    // Cari named pipe dari helper lu
    PipeBuffer* pipe = GetNamedPipeBuffer(path);
    if (pipe) return (UPTR)pipe; // Alamat memori jadi InodeID
    return 0;
}

BOOL PipeFileSystem::PopulateInode(U64 InodeID, ::Inode* vfsNode){
    if (InodeID == 0 || !vfsNode) return FALSE;
        PipeBuffer* pipe = (PipeBuffer*)(UPTR)InodeID;

    vfsNode->Type = FT_PIPE;
    vfsNode->FSOwner = this;
    vfsNode->FileSize = pipe->BytesAvailable; 
    vfsNode->InodeID = InodeID;
    return TRUE;
}

U64 PipeFileSystem::CreateNode(const char* path, U32 Flags){
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

U32 PipeFileSystem::Read(File* file, U8* Buffer, U32 Size){
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

U32 PipeFileSystem::Write(File *file, U8 *Buf, U32 Size){
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

                if(file->Flags & O_NONBLOCK) {
                    buf->Lock.Release();
                    return WriteCount; // Return seberapa banyak yg berhasil ditulis sblm penuh
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

VOID PipeFileSystem::Close(File *file){
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

    // 2. Logic Memory Management - NEBENG VFS!
    // Karena VFS belum ngurangin RefCount pas manggil fungsi ini,
    // kalau angkanya 1, berarti ini adalah close TERAKHIR.
    BOOL shouldDelete = (file->Node->RefCount == 1);

    buf->Lock.Release();

    // Hapus memory-nya! (Sesuai keinginan lu, auto-delete pas tutup total)
    if (shouldDelete) {
        RemoveNamedPipeEntry(buf);
        delete buf; 
    }
}

BOOL PipeFileSystem::Stat(const char* path, FileInfo* info) {
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

short PipeFileSystem::Poll(File *file, short events){
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

namespace PipeFS{
    BOOL Init(){
        g_PipeFileSystemInstance = new PipeFileSystem();
        if(VFSManager::MountFS("/pipe", g_PipeFileSystemInstance)){
            Printk::Write(Printk::Level::LOG_ALERT, "PIPE INSTANCE MEMORY: %p.\n", g_PipeFileSystemInstance);
            return TRUE;
        }
        return FALSE;
    }
}