#pragma once
#include "ext2.hpp"
#include <rosval.h>

class Partition;

class EXT2FileSystem : public FileSystem{
    private:
        Partition *m_Partition;
        EXT2::SuperBlock m_Superblock;
        U32 m_BlockSize;
    public:
        EXT2FileSystem();
        virtual ~EXT2FileSystem();

        virtual BOOL Mount(Partition *Part) override;
        virtual File* Open(const char* path) override;
        virtual File* Create(const char *Path) override;
        virtual void Close(File* file) override;
        virtual U32 Read(File* file, U8* buffer, U32 size) override; 
        virtual U32 Write(File *File, U8 *Buffer, U32 Size) override;
        virtual BOOL Delete(const char* path) override;
        virtual BOOL Rename(const char* oldPath, const char* newPath) override;
        virtual BOOL Seek(File* file, U64 position) override;
};