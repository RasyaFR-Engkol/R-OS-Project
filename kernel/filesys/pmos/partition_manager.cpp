#include <rossys.hpp>
#include <rosval.h>
#define PRINTK_MODULE_NAME "PartManager"
#include <logging.hpp>
#include <string.hpp>
#include <filesystem/filesystem.hpp>
#include "partition_manager.hpp"
#include "../vfs/vfs.hpp"

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

    PARTMANAGER InitializeRegisteredPartitionToFS(){
        Printk::Write(Printk::Level::LOG_INFO, " Partition Manager: Initializing registered partitions to filesystems...\n");

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
                //String::Strcat(MountParh, String::Itoa(i, MountPath, 10));
                switch(i){
                    case 0: String::Strcat(MountPath, "0"); break;
                    case 1: String::Strcat(MountPath, "1"); break;
                    case 2: String::Strcat(MountPath, "2"); break;
                    case 3: String::Strcat(MountPath, "3"); break;
                    case 4: String::Strcat(MountPath, "4"); break;
                    case 5: String::Strcat(MountPath, "5"); break;
                    case 6: String::Strcat(MountPath, "6"); break;
                    case 7: String::Strcat(MountPath, "7"); break;
                    case 8: String::Strcat(MountPath, "8"); break;
                    case 9: String::Strcat(MountPath, "9"); break;
                    default: String::Strcat(MountPath, "X"); break; // for simplicity
                }

                if(VFSManager::Mount(MountPath, Part)){
                    Printk::Write(Printk::Level::LOG_INFO, "  Mounted partition %d at %s\n", i, MountPath);
                    MountedCount++;
                } else {
                    Printk::Write(Printk::Level::LOG_ERR, "  Failed to mount partition %d at %s\n", i, MountPath);
                }
            }
        }

        Printk::Write(Printk::Level::LOG_INFO, " Partition Manager: Mounted %d out of %d registered partitions.\n", MountedCount, Count);
        return PARTITIONS_INITIALIZED;
    }

    BOOL RegisterPartition(Partition *Part){
        if(!Part){
            Printk::Write(Printk::Level::LOG_ERR, " Partition Manager: Invalid Partition Entry.\n");
            return FALSE;
        }

        if(s_PartitionsCount >= MAX_PARTITIONS){
            Printk::Write(Printk::Level::LOG_ERR, " Partition Manager: Maximum partition limit reached.\n");
            return FALSE;
        }

        s_PartitionsList[s_PartitionsCount] = Part;
        s_PartitionsCount++;

        Printk::Write(Printk::Level::LOG_INFO, "Partition Manager: Registered new partition. No. %d (LBA: %llu, Sectors: %llu)\n",
            s_PartitionsCount,
            Part->GetStartingLBA(),
            Part->GetSectorCount()
        );

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