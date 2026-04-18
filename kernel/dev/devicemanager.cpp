#include "export_sym.hpp"
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
    DevOBJManaager ObjectManager;

    BOOL RegisterBlockDevice(IBlockDevice *Device){
        if(g_BlockDeviceCount >= MAX_BLOCK_DEVICE){
            Printk::Write(Printk::Level::LOG_ERR, " DeviceManager: Maximum block device limit reached.\n");
            return FALSE;
        }
        g_BlockDevices[g_BlockDeviceCount] = Device;
        g_BlockDeviceCount++;

        return TRUE;
    }

    BOOL RegisterCharDevice(ICharDevice *Device){
        if(g_CharDeviceCount >= MAX_CHAR_DEVICE){ // Gunakan count & max yang benar
            Printk::Write(Printk::Level::LOG_ERR, " DeviceManager: Maximum char device limit reached.\n");
            return FALSE;
        }
        g_CharDevices[g_CharDeviceCount] = Device;
        g_CharDeviceCount++;
        return TRUE;
    }

    U32 GetBlockDeviceCount(){
        return g_BlockDeviceCount;
    }   

        BOOL UnregisterBlockDevice(IBlockDevice *Device){
            if(!Device) return FALSE;
            // Find device
            int idx = -1;
            for(U32 i = 0; i < g_BlockDeviceCount; i++){
                if(g_BlockDevices[i] == Device){ idx = (int)i; break; }
            }
            if(idx < 0) return FALSE;
            // Shift remaining entries down
            for(U32 i = (U32)idx; i + 1 < g_BlockDeviceCount; i++){
                g_BlockDevices[i] = g_BlockDevices[i+1];
            }
            g_BlockDevices[g_BlockDeviceCount - 1] = nullptr;
            g_BlockDeviceCount--;
            Printk::Write(Printk::Level::LOG_INFO, " DeviceManager: Unregistered block device='%s'.\n", Device->GetDeviceName());
            return TRUE;
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