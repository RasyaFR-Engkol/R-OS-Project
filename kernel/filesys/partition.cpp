#include "partition.hpp"
#include "filesystem.hpp"
#include <mm.hpp>
#define PRINTK_MODULE_NAME "Partition"
#include <logging.hpp>
#include "vfs/vfs.hpp"
#include "ext2/ext2.hpp"
#include "rootfs/rootfs.hpp"


BOOL Partition::Mount(){
    if(m_isMounted){
        return TRUE;
    }

    Printk::Write(Printk::Level::LOG_INFO, " Partition: Mounting partition (LBA: %llu, Sectors: %llu)...\n",
        m_StartingLBA,
        m_SectorCount
    );

    PageAlloc::DMAAlloc::DMABuffer* BootSectorBuffer = nullptr;
    if(!this->ReadSectors(0, 1, &BootSectorBuffer)){
        Printk::Write(Printk::Level::LOG_ERR, " Partition: Failed to read boot sector for mounting.\n");
        return FALSE;
    }

    U8 *BootSector = (U8*)BootSectorBuffer->VirtAddr;

    const char *DetectedFSType = nullptr;

    if (String::Memcmp(BootSector + 0x52, "FAT32   ", 8) == 0) {
        Printk::Write(Printk::Level::LOG_INFO, "  Detected FAT32 filesystem.\n");
        DetectedFSType = "FAT32";
    }

    PageAlloc::DMAAlloc::FreeDMABuffer(BootSectorBuffer);
    BootSectorBuffer = nullptr;

    if(!DetectedFSType){
        Printk::Write(Printk::Level::LOG_WARNING, " NOT FAT32. Trying attempt to detect another partition..\n");
        if(!this->ReadSectors(2, 1, &BootSectorBuffer)){
            Printk::Write(Printk::Level::LOG_ERR, " Partition: Failed to read sector 2 for mounting.\n");
            return FALSE;
        }

        EXT2::SuperBlock *SB = (EXT2::SuperBlock*)BootSectorBuffer->VirtAddr;

        if(SB->s_magic == EXT2_SUPER_MAGIC){
            Printk::Write(Printk::Level::LOG_INFO, "  Detected EXT2 filesystem.\n");
            DetectedFSType = "EXT2";
        }

        PageAlloc::DMAAlloc::FreeDMABuffer(BootSectorBuffer);
        BootSectorBuffer = nullptr;
    }

    if(!DetectedFSType){
        Printk::Write(Printk::Level::LOG_WARNING, " Partition: Unable to detect filesystem type.\n");
        return FALSE;
    }

    m_Filesystem = VFSManager::InstantiateDriver(DetectedFSType);
    if(!m_Filesystem){
        Printk::Write(Printk::Level::LOG_ERR, " Partition: Failed to instantiate filesystem driver for type %s.\n", DetectedFSType);
        return FALSE;
    }

    if(m_Filesystem->Mount(this)){
        m_isMounted = TRUE;
        return TRUE;
    } else {
        Printk::Write(Printk::Level::LOG_ERR, " Partition: Filesystem driver failed to mount partition.\n");
        m_Filesystem = nullptr;
        return FALSE;
    }

    Printk::Write(Printk::Level::LOG_WARNING, " Partition: Unsupported or unrecognized filesystem.\n");
    return FALSE;
}
