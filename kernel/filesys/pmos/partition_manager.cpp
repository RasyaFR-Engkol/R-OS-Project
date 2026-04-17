#include <rossys.hpp>
#include <rosval.h>
#define PRINTK_MODULE_NAME "PartManager"
#include <logging.hpp>
#include <string.hpp>
#include <filesystem/filesystem.hpp>
#include "partition_manager.hpp"
#include "../vfs/vfs.hpp"
#include <../kernel/filesys/iblockdevice.hpp>
#include <../kernel/dev/devicemanager.hpp>
#include <../kernel/filesys/devfs/devfs.hpp>

namespace PartitionManager{
    static const U32 MAX_PARTITIONS = 128;

    static Partition* s_PartitionsList[MAX_PARTITIONS];

    static U32 s_PartitionsCount = 0;

    void InitializePM(){
        for(U32 i = 0; i < MAX_PARTITIONS; i++){
            s_PartitionsList[i] = nullptr;
        }
        s_PartitionsCount = 0;

        Printk::Write(Printk::Level::LOG_NOTICE, "PartManager v1.0 Copyright ROS Team (This is part of ManagementPMTools by ROS Team).\n");
    }

    Partition *FindPartitionByDeviceName(const char *DeviceName){
        for(U32 i = 0; i < s_PartitionsCount; i++){
            if(s_PartitionsList[i] && String::Strcmp(s_PartitionsList[i]->GetPartitionName(), DeviceName) == 0){
                return s_PartitionsList[i];
            }
        }
        return nullptr;
    }

    PARTMANAGER InitializeRegisteredPartitionToFS(){
        //Printk::Write(Printk::Level::LOG_INFO, " Partition Manager: Initializing registered partitions to filesystems...\n");

        U32 Count = GetPartitionCount();
        if(Count == 0){
            Printk::Write(Printk::Level::LOG_WARNING, " Partition Manager: No registered partitions found.\n");
            return NO_PARTITIONS;
        }

        U32 MountedCount = 0;
        for(U32 i = 0; i < Count; i++){
            Partition *Part = GetPartitionByIndex(i);
            if(Part && Part->Mount()){
                char MountPath[64];
                String::Strcpy(MountPath, "/mnt/part");
                
                char numbuf[16];
                String::Utoa((unsigned long long)(i+1), (char*)numbuf, 10); // i+1 agar partisi pertama jadi /mnt/part1
                String::Strcat(MountPath, numbuf);

                if(VFSManager::Mount(MountPath, Part)){
                    Printk::Write(Printk::Level::LOG_INFO, "  Mounted partition %d at %s\n", i, MountPath);
                    MountedCount++;
                } else {
                    Printk::Write(Printk::Level::LOG_ERR, "  Failed to mount partition %d at %s\n", i, MountPath);
                }
            }
        }

        //Printk::Write(Printk::Level::LOG_INFO, " Partition Manager: Mounted %d out of %d registered partitions.\n", MountedCount, Count);
        return PARTITIONS_INITIALIZED;
    }

    BOOL RegisterPartition(Partition *Part, IBlockDevice *ParentDevice) {
        if(!Part || s_PartitionsCount >= MAX_PARTITIONS) return FALSE;

        // Gunakan count saat ini + 1 untuk index penamaan (misal sda1)
        U32 partIndex = s_PartitionsCount + 1;

        PartitionBlockDevice *Wrapper = new PartitionBlockDevice(Part, ParentDevice->GetDeviceName(), partIndex);
        if(!Wrapper) return FALSE;

        // Registrasi
        DeviceManager::RegisterBlockDevice(Wrapper);
        Part->SetDeviceWrapper(Wrapper);

        // Registrasi ke DevFS
        FileSystem *FS = nullptr; CHAR8 rel[256];
        if(VFSManager::ResolvePath("/dev", &FS, rel) && FS) {
            ((DevFS*)FS)->RegisterBlockDevice(Wrapper, Wrapper->GetDeviceName());
        }

        // Baru simpan ke list dan naikkan count kalau semua ok
        s_PartitionsList[s_PartitionsCount] = Part;
        s_PartitionsList[s_PartitionsCount]->SetDeviceName(Wrapper->GetDeviceName());
        s_PartitionsCount++;

        return TRUE;
    }

    Partition* GetPartitionByIndex(U32 index) {
        // Cek batas
        if (index >= s_PartitionsCount) {
            return nullptr;
        }
        return s_PartitionsList[index];
    }

    U32 GetPartitionCount() {
        return s_PartitionsCount;
    }
}