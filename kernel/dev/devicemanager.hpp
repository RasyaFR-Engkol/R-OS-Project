#pragma once

#include "../filesys/iblockdevice.hpp"
#include <logging.hpp>

#define MAX_BLOCK_DEVICE 16

namespace DeviceManager{
    extern IBlockDevice *g_BlockDevices[MAX_BLOCK_DEVICE];
    extern U32 g_BlockDeviceCount;

    BOOL RegisterBlockDevice(IBlockDevice *Device);

    U32 GetBlockDeviceCount();

    IBlockDevice *GetBlockDevice(U32 Index);
}