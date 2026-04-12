#include <rosval.h>
#include "../vfs/vfs.hpp"

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
    U64 Lookup(const char* path) override;
    BOOL PopulateInode(U64 InodeID, ::Inode* vfsNode) override;
    U64 CreateNode(const char* path, U32 Flags) override;
    // ==============================================================

    // LOGIC BACA
    U32 Read(File* file, U8* Buffer, U32 Size) override;
    // LOGIC TULIS
    U32 Write(File *file, U8 *Buf, U32 Size) override;

    // LOGIC CLOSE (RAM Friendly!)
    void Close(File* file) override ;

    // Dummy overrides
    BOOL Delete(const char* path) override { return FALSE; }
    BOOL Rename(const char* o, const char* n) override { return FALSE; }
    BOOL Seek(File* f, U64 p, U32 o) override { return FALSE; }
    BOOL Truncate(File* f, U64 s) override { return FALSE; }
    BOOL MKDir(const char* path) override { return FALSE; }
    BOOL RMDir(const char* path) override { return FALSE; }
    BOOL Flush(File* file) override { return TRUE; }
    BOOL Append(File* file, U8* buffer, U32 size) override { return FALSE; }
    BOOL Stat(const char* path, FileInfo* info) override;
    INTN ReadDir(File* dirFile, void* buffer, U32 bufferSize) override { return -1; }
    short Poll(File* file, short events) override;
};

namespace PipeFS{
    BOOL Init();
}