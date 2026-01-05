#pragma once
#include <filesystem/filesystem.hpp>
#include "../vfs/vfs.hpp"

class RootFS : public FileSystem{
    private:
        FileSystem *m_BackendFS;
        File *m_BackendRootHandle;
    public:
        RootFS();
        virtual ~RootFS();

        VOID SetBackingFileSystem(FileSystem *fs);

        // Mount logic
        virtual BOOL Mount(Partition *Part) override { return TRUE; }

        File* Open(const char* path, U32 Flags) override;
        U32 Read(File* file, U8* buffer, U32 size) override {return 0;};
        U32 Write(File *File, U8 *Buffer, U32 Size) override {return 0;};

        INTN ReadDir(File* dirFile, void* buffer, U32 bufferSize) override;

        void Close(File* file) override { if(file) delete file; } // Atau Kfree tergantung allocator lu
        BOOL Delete(const char* path) override { return FALSE; }
        BOOL Rename(const char* o, const char* n) override { return FALSE; }
        BOOL Seek(File* f, U64 p, U32 o) override { return FALSE; }
        BOOL Truncate(File* f, U64 s) override { return FALSE; }
        BOOL MKDir(const char* path) override { return FALSE; } // Nanti bisa diupgrade jadi RAMFS beneran
        BOOL RMDir(const char* path) override { return FALSE; }
        BOOL Flush(File* file) override { return TRUE; }
        BOOL Append(File* file, U8* buffer, U32 size) override { return FALSE; }
        BOOL Unmount() override {return true;}
        BOOL Stat(const char* path, FileInfo* info) override {return true;}
};

namespace ROOTFS{
    VOID InitROOTFS();
    RootFS *GetRootFS();
}