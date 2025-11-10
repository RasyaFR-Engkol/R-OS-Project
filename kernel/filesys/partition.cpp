#include "partition.hpp"
#include <mm.hpp>
#define PRINTK_MODULE_NAME "Partition"
#include <logging.hpp>
#include "vfs/vfs.hpp"


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
        Printk::Write(Printk::Level::LOG_INFO, " Partition: Mounted successfully with %s filesystem.\n", DetectedFSType);
        return TRUE;
    } else {
        Printk::Write(Printk::Level::LOG_ERR, " Partition: Filesystem driver failed to mount partition.\n");
        m_Filesystem = nullptr;
        return FALSE;
    }

    Printk::Write(Printk::Level::LOG_WARNING, " Partition: Unsupported or unrecognized filesystem.\n");
    return FALSE;
}
