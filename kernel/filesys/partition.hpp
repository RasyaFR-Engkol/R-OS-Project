#pragma once

#include "gpt/gpt.hpp"
#include "string.hpp"
#include <logging.hpp>
#include "../log/printk/printk.hpp"
#include "iblockdevice.hpp"
#include "devfs/devfs.hpp"
// Avoid including vfs.hpp here to prevent circular include (vfs.hpp includes partition.hpp).
// Forward-declare the ResolvePath helper we use in the destructor.
namespace VFSManager { BOOL FindMountPoint(const char *path, FileSystem** outFS, char *OutRelativePath); }
namespace DeviceManager{ BOOL UnregisterBlockDevice(IBlockDevice *Device); }

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
    // Optional wrapper object registered with DeviceManager/DevFS
    IBlockDevice* m_DeviceWrapper;

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
        m_DeviceWrapper = nullptr;
    }

    ~Partition(){
        // Unregister device wrapper if present
        if(m_DeviceWrapper){
            // Try remove from DevFS first
            FileSystem* fs = nullptr; char rel[256];
            if (VFSManager::FindMountPoint((const char*)"/dev", &fs, rel) && fs) {
                DevFS* devfs = (DevFS*)fs;
                devfs->UnregisterDevice(m_DeviceWrapper->GetDeviceName());
            }
            // Then remove from DeviceManager
            DeviceManager::UnregisterBlockDevice(m_DeviceWrapper);
            // Free wrapper object: Partition now owns the wrapper and is
            // responsible for deleting it after unregistering.
            delete m_DeviceWrapper;
            m_DeviceWrapper = nullptr;
        }
    }

    // Store pointer to wrapper device representing this partition
    void SetDeviceWrapper(IBlockDevice* wrapper){ m_DeviceWrapper = wrapper; }
    IBlockDevice* GetDeviceWrapper(){ return m_DeviceWrapper; }

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
            // Partition is read-only; avoid calling Printk from a header-level
            // inline method to prevent circular include issues during build.
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