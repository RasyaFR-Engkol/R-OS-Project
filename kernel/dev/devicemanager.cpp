#include "string.hpp"
#include <rosval.h>
#define PRINTK_MODULE_NAME "DevMGR"
#include <logging.hpp>
#include "../filesys/iblockdevice.hpp"
#include "devicemanager.hpp"
#include "../filesys/devfs/devfs.hpp"

namespace DeviceManager{
    IBlockDevice *g_BlockDevices[MAX_BLOCK_DEVICE];
    U32 g_BlockDeviceCount = 0;

    ICharDevice *g_CharDevices[MAX_CHAR_DEVICE];
    U32 g_CharDeviceCount = 0;

    BOOL RegisterBlockDevice(IBlockDevice *Device){
        if(g_BlockDeviceCount >= MAX_BLOCK_DEVICE){
            Printk::Write(Printk::Level::LOG_ERR, " DeviceManager: Maximum block device limit reached.\n");
            return FALSE;
        }
        g_BlockDevices[g_BlockDeviceCount] = Device;
        g_BlockDeviceCount++;
        Printk::Write(Printk::Level::LOG_INFO, " DeviceManager: Registered block device. Total devices: %u\n", g_BlockDeviceCount);
        // Extra debug: print addresses to detect duplicate/ODR issues or memory corruption
        Printk::Write(Printk::Level::LOG_DEBUG, " DeviceManager: Debug addr g_BlockDeviceCount=%p g_BlockDevices=%p device=%p\n",
            (void*)&g_BlockDeviceCount,
            (void*)g_BlockDevices,
            (void*)Device
        );

        return TRUE;
    }

    BOOL RegisterCharDevice(ICharDevice *Device){
        if(g_CharDeviceCount >= MAX_CHAR_DEVICE){ // Gunakan count & max yang benar
            Printk::Write(Printk::Level::LOG_ERR, " DeviceManager: Maximum char device limit reached.\n");
            return FALSE;
        }
        g_CharDevices[g_CharDeviceCount] = Device;
        g_CharDeviceCount++;
        Printk::Write(Printk::Level::LOG_INFO, " DeviceManager: Registered char device '%s'.\n", Device->GetDeviceName());
        return TRUE;
    }

    U32 GetBlockDeviceCount(){
        Printk::Write(Printk::Level::LOG_DEBUG, " DeviceManager: Current block device count: %u\n", g_BlockDeviceCount);
        // Extra debug: print addresses to correlate with register-time addresses
        Printk::Write(Printk::Level::LOG_DEBUG, " DeviceManager: Debug addr (query) g_BlockDeviceCount=%p g_BlockDevices=%p\n",
            (void*)&g_BlockDeviceCount,
            (void*)g_BlockDevices
        );
        return g_BlockDeviceCount;
    }   

    IBlockDevice *GetBlockDevice(U32 Index){
        if(Index >= g_BlockDeviceCount){
            return nullptr;
        }
        return g_BlockDevices[Index];
    }

    IBlockDevice* FindBlockDevice(const char* name) {
        if (!name) return nullptr;
        for (U32 i = 0; i < g_BlockDeviceCount; i++) {
            if (g_BlockDevices[i] && String::Strcmp(g_BlockDevices[i]->GetDeviceName(), name) == 0) {
                return g_BlockDevices[i];
            }
        }
        return nullptr;
    }

    ICharDevice* FindCharDevice(const char* name) {
        if (!name) return nullptr;
        for (U32 i = 0; i < g_CharDeviceCount; i++) {
            if (g_CharDevices[i] && String::Strcmp(g_CharDevices[i]->GetDeviceName(), name) == 0) {
                return g_CharDevices[i];
            }
        }
        return nullptr;
    }
}