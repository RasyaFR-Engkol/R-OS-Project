#pragma once

#include "../filesys/iblockdevice.hpp"
#include <logging.hpp>

class ICharDevice;

#define MAX_BLOCK_DEVICE 16
#define MAX_CHAR_DEVICE 16

namespace DeviceManager{
    extern IBlockDevice *g_BlockDevices[MAX_BLOCK_DEVICE];
    extern U32 g_BlockDeviceCount;

    extern ICharDevice *g_CharDevices[MAX_CHAR_DEVICE];
    extern U32 g_CharDeviceCount;

    BOOL RegisterBlockDevice(IBlockDevice *Device);

    BOOL RegisterCharDevice(ICharDevice *Device);

    U32 GetBlockDeviceCount();
    IBlockDevice *GetBlockDevice(U32 Index);

    IBlockDevice* FindBlockDevice(const char* name);
    ICharDevice* FindCharDevice(const char* name);
}