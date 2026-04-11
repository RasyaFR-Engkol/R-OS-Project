#pragma once

#include <rosval.h>
#include <filesystem/filesystem.hpp>

class Partition;

class ttyFB0 : public FileSystem{
    public:
        ttyFB0();
        virtual ~ttyFB0();

        virtual U32 Read(File* file, U8* buffer, U32 size) override; 
        virtual U32 Write(File *File, U8 *Buffer, U32 Size) override;
        virtual void Close(File* file) override;
};

namespace FBDriver{
    BOOL RegisterFBToDevFS(DevFS* devfs);
}