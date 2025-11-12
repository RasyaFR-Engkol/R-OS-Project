#pragma once

#include <rosval.h>
#include <mm.hpp>

class IBlockDevice{
    public:
        virtual ~IBlockDevice() {}

    virtual BOOL ReadSectors(U64 LBA, U32 Count, PageAlloc::DMAAlloc::DMABuffer **BufferOut) = 0;

    virtual BOOL WriteSectors(U64 LBA, U32 Count, PageAlloc::DMAAlloc::DMABuffer *Buffer) = 0;

    virtual const CHAR8* GetDeviceName() = 0;
};