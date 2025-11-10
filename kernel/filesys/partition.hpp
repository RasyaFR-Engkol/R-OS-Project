#pragma once

#include "gpt/gpt.hpp"
#include "string.hpp"
#include <filesystem/filesystem.hpp>
#include <logging.hpp>

class FileSystem;

class Partition{
    private:
        // Info partisi
    U64 m_StartingLBA;      // LBA absolut di disk
    U64 m_EndingLBA;
    U64 m_SectorCount;
    GPTFS::GPTGuid m_TypeGUID;
    GPTFS::GPTGuid m_UniqueGUID;
    
    // Info state/driver
    AHCI::AHCIDriver m_BlockDevice; // Pointer ke driver block device (AHCI)
    U8 m_PortNumber; // Port AHCI tempat partisi ini berada
    FileSystem* m_Filesystem; // Pointer ke driver FS yang ter-mount
    BOOL m_isMounted;

    public:
    Partition(GPTFS::GptPartitionEntry *RawEntry, AHCI::AHCIDriver &Device, U8 PortNum){
        m_StartingLBA = RawEntry->StartingLBA;
        m_EndingLBA = RawEntry->EndingLBA;
        m_SectorCount = (m_EndingLBA - m_StartingLBA) + 1;
        m_TypeGUID = RawEntry->PartitionTypeGUID;
        m_UniqueGUID = RawEntry->UniquePartitionGUID;

        m_BlockDevice = Device;
        m_PortNumber = PortNum;
        m_Filesystem = nullptr;
        m_isMounted = FALSE;
    }

    ~Partition(){
        // Untuk unmount nanti
    }

    BOOL ReadSectors(U64 LBA, U32 Count, PageAlloc::DMAAlloc::DMABuffer **BufferOut){
        if(LBA + Count > m_SectorCount){
            return FALSE;
        }

        U64 AbsoluteLBA = m_StartingLBA + LBA;
        // AHCI read is a free function that takes (AHCIDriver&, PortNum, lba, count, outBuf)
        return AHCI::ReadSectors(m_BlockDevice, m_PortNumber, AbsoluteLBA, Count, BufferOut);
    }

    BOOL WriteSectors(U64 LBA, U32 Count, PageAlloc::DMAAlloc::DMABuffer *Buffer){
        // Bounds-check: make sure the requested range fits inside this partition
        if(LBA + Count > m_SectorCount){
            return FALSE;
        }

        U64 AbsoluteLBA = m_StartingLBA + LBA;
        // AHCI write is a free function that takes (AHCIDriver&, PortNum, lba, count, buf)
        return AHCI::WriteSectors(m_BlockDevice, m_PortNumber, AbsoluteLBA, Count, Buffer);
    }

    BOOL Mount();

    U64 GetStartingLBA() { return m_StartingLBA; }
    U64 GetSectorCount() { return m_SectorCount; }
    GPTFS::GPTGuid GetTypeGUID() { return m_TypeGUID; }
    FileSystem* GetFilesystem() { return m_Filesystem; }
};