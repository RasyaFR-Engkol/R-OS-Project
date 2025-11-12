#pragma once

#include "gpt/gpt.hpp"
#include "string.hpp"
#include <filesystem/filesystem.hpp>
#include <logging.hpp>
#include "iblockdevice.hpp"

class FileSystem;
class IBlockDevice;

class Partition{
    private:
        // Info partisi
    U64 m_StartingLBA;      // LBA absolut di disk
    U64 m_EndingLBA;
    U64 m_SectorCount;
    GPTFS::GPTGuid m_TypeGUID;
    GPTFS::GPTGuid m_UniqueGUID;
    
    // Info state/driver
    IBlockDevice *m_ParentDevice;

    FileSystem* m_Filesystem; // Pointer ke driver FS yang ter-mount
    BOOL m_isMounted;
    BOOL m_ReadOnly;

    public:
    Partition(GPTFS::GptPartitionEntry *RawEntry, IBlockDevice* ParentDevice) {
        m_StartingLBA = RawEntry->StartingLBA;
        m_EndingLBA = RawEntry->EndingLBA;
        m_SectorCount = (m_EndingLBA - m_StartingLBA) + 1;
        m_TypeGUID = RawEntry->PartitionTypeGUID;
        m_UniqueGUID = RawEntry->UniquePartitionGUID;

        m_ParentDevice = ParentDevice; // Simpan pointer ke perangkat induk
        m_Filesystem = nullptr;
        m_isMounted = FALSE;
        m_ReadOnly = TRUE;
    }

    ~Partition(){
        // Untuk unmount nanti
    }

    BOOL ReadSectors(U64 LBA, U32 Count, PageAlloc::DMAAlloc::DMABuffer **BufferOut) {
        if (LBA + Count > m_SectorCount) {
            return FALSE; // Cek batas partisi
        }

        // Terjemahkan LBA relatif partisi ke LBA absolut disk
        U64 AbsoluteLBA = m_StartingLBA + LBA;

        //Printk::Write(Printk::Level::LOG_INFO, "Partition: ReadSectors LBA %llu (abs %llu), Count %u\n", LBA, AbsoluteLBA, Count);
        
        // Delegasikan ke perangkat induk
        return m_ParentDevice->ReadSectors(AbsoluteLBA, Count, BufferOut);
    }

    // --- PERUBAHAN DI SINI ---
    BOOL WriteSectors(U64 LBA, U32 Count, PageAlloc::DMAAlloc::DMABuffer *Buffer) {
        if(m_ReadOnly){
            Printk::Write(Printk::Level::LOG_ERR, "Partition: WriteSectors failed - partition is read-only\n");
            return FALSE;
        }

        if (LBA + Count > m_SectorCount) {
            return FALSE;
        }

        U64 AbsoluteLBA = m_StartingLBA + LBA;

        //Printk::Write(Printk::Level::LOG_INFO, "Partition: WriteSectors LBA %llu (abs %llu), Count %u\n", LBA, AbsoluteLBA, Count);
        
        // Delegasikan ke perangkat induk
        return m_ParentDevice->WriteSectors(AbsoluteLBA, Count, Buffer);
    }

    BOOL Mount();

    VOID SetReadOnly(){ m_ReadOnly = TRUE;}
    VOID SetReadWrite() { m_ReadOnly = FALSE; }

    U64 GetStartingLBA() { return m_StartingLBA; }
    U64 GetSectorCount() { return m_SectorCount; }
    GPTFS::GPTGuid GetTypeGUID() { return m_TypeGUID; }
    FileSystem* GetFilesystem() { return m_Filesystem; }
};