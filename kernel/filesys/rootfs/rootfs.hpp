#pragma once
#include <filesystem/filesystem.hpp>
#include "../vfs/vfs.hpp"

class RootFS : public FileSystem{
    private:
        FileSystem *m_BackendFS;
    public:
        RootFS();
        virtual ~RootFS();

        VOID SetBackingFileSystem(FileSystem *fs);

        // Mount logic
        virtual BOOL Mount(Partition *Part) override { return TRUE; }

        virtual U64 Lookup(const char* path) override;
        virtual BOOL PopulateInode(U64 InodeID, ::Inode* vfsNode) override;
        virtual U64 CreateNode(const char* path, U32 Flags) override { return 0; }
        U32 Read(File* file, U8* buffer, U32 size) override;
        U32 Write(File *File, U8 *Buffer, U32 Size) override;

        INTN ReadDir(File* dirFile, void* buffer, U32 bufferSize) override;

        void Close(File* file) override { return; } // RootFS gak punya state apa apa, jadi Close cuman no-op
        BOOL Delete(const char* path) override { return FALSE; }
        BOOL Rename(const char* o, const char* n) override { return FALSE; }
        BOOL Seek(File* f, U64 p, U32 o) override { return FALSE; }
        BOOL Truncate(File* f, U64 s) override { return FALSE; }
        BOOL MKDir(const char* path) override { return FALSE; } // Nanti bisa diupgrade jadi RAMFS beneran
        BOOL RMDir(const char* path) override { return FALSE; }
        BOOL Flush(File* file) override { return TRUE; }
        BOOL Append(File* file, U8* buffer, U32 size) override { return FALSE; }
        BOOL Unmount() override {return true;}
        BOOL Stat(const char* path, FileInfo* info) override;
};

namespace ROOTFS{
    VOID InitROOTFS();
    RootFS *GetRootFS();
}