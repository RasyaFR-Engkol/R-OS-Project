#define PRINTK_MODULE_NAME "GPTFS"
#include "gpt.hpp"
#include <rosval.h>
#include <rossys.hpp>
#include <filesystem/filesystem.hpp>
#include <mm.hpp>
#include <logging.hpp>
#include <string.hpp>
#include "../pmos/partition_manager.hpp"
#include "../partition.hpp"
#include "../../mm/kmalloc/kmalloc.hpp"
#include "../../driver/ahci/ahci_disk.hpp"
#include "../../dev//devicemanager.hpp"
#include "../devfs/devfs.hpp"
#include "../vfs/vfs.hpp"

namespace GPTFS{
    #define SECTOR_SIZE 512

    BOOL InitializeGPT(IBlockDevice *Device, GPTFS::GPTHeader **OutHeader){
        // Kita bisa nyari AHCI Driver nya sendiri ajah
        // Kita coba baca LBA 1 (GPT Header)
        PageAlloc::DMAAlloc::DMABuffer *buf = nullptr;

        if(!Device->ReadSectors(1, 1, &buf)){
            Printk::Write(Printk::Level::LOG_ERR, " GPT: Failed to read GPT Header from device.\n");
            return FALSE;
        }


        if(!buf){
            Printk::Write(Printk::Level::LOG_ERR, " GPT: AHCI::ReadSectors returned NULL buffer for header read.\n");
            return FALSE;
        }

            // Free Buff, kita pake KMALLOC nanti
            *OutHeader = (GPTFS::GPTHeader*)Kmalloc::Alloc(sizeof(GPTFS::GPTHeader));
            if(!*OutHeader){
                Printk::Write(Printk::Level::LOG_ERR, " GPT: Failed to allocate memory for GPT Header\n");
                PageAlloc::DMAAlloc::FreeDMABuffer(buf);
                return FALSE;
            }

            String::Memcpy((VOID*)*OutHeader, (VOID*)buf->VirtAddr, sizeof(GPTFS::GPTHeader));
            // Kita bisa free DMA Buffer karena udah disalin
            PageAlloc::DMAAlloc::FreeDMABuffer(buf);
            // Dump GPT:
            //Printk::Write(Printk::Level::LOG_INFO, "GPT OK\n");
            // Signature is stored as a U64 ("EFI PART" in little-endian).
            // Don't pass the U64 value as a char* to printf — that causes a crash.
            char SigBuf[9];
            // Copy the 8 signature bytes and NUL-terminate for safe printing
            String::Memcpy(SigBuf, (const void*)&((*OutHeader)->Signature), 8);
            SigBuf[8] = '\0';
            //Printk::Write(Printk::Level::LOG_DEBUG, " Signature: %.8s\n", SigBuf);
            //Printk::Write(Printk::Level::LOG_DEBUG, " Revision: %08X\n", (*OutHeader)->Revision);
            //Printk::Write(Printk::Level::LOG_DEBUG, " Header Size: %u bytes\n", (*OutHeader)->HeaderSize);
            //Printk::Write(Printk::Level::LOG_DEBUG, " Backup LBA: %llu\n", (*OutHeader)->BackupLBA);
            //Printk::Write(Printk::Level::LOG_DEBUG, " First Us   able LBA: %llu\n", (*OutHeader)->FirstUsableLBA);
            //Printk::Write(Printk::Level::LOG_DEBUG, " Last Usable LBA: %llu\n", (*OutHeader)->LastUsableLBA);
            //Printk::Write(Printk::Level::LOG_DEBUG, " Partition Entry LBA: %llu\n", (*OutHeader)->PartitionEntryLBA);
            //Printk::Write(Printk::Level::LOG_DEBUG, " Number of Partition Entries: %u\n", (*OutHeader)->NumberOfPartitionEntries);
            //Printk::Write(Printk::Level::LOG_DEBUG, " Size of Partition Entry: %u bytes\n", (*OutHeader)->SizeOfPartitionEntry);
            return TRUE;
    }
    
    BOOL ParsePartitionEntries(IBlockDevice* Device, GPTFS::GPTHeader* GPTHeader0){
        if(!GPTHeader0){
            Printk::Write(Printk::Level::LOG_ERR, " GPT: GPT Header not initialized.\n");
            return FALSE;
        }

        // PERBAIKAN:
        U32 TableSizeBytes = GPTHeader0->NumberOfPartitionEntries * GPTHeader0->SizeOfPartitionEntry;
            U32 SectorsToRead = (TableSizeBytes + (SECTOR_SIZE - 1)) / SECTOR_SIZE; // (pembulatan ke atas)

        // Alokasikan jumlah byte yang TEPAT (atau sedikit lebih)
        // 'TableSizeBytes' sudah merupakan jumlah byte yang kita butuhkan.
        // Kita bisa alokasikan 'SectorsToRead * SECTOR_SIZE' agar pas dengan ukuran sektor.
        if(SectorsToRead == 0){
            Printk::Write(Printk::Level::LOG_ERR, " GPT: Partition entry table size invalid (0 sectors).\n");
            return FALSE;
        }

        PageAlloc::DMAAlloc::DMABuffer *buf = nullptr;
        if(!Device->ReadSectors(GPTHeader0->PartitionEntryLBA, SectorsToRead, &buf)){
            if(buf){
                PageAlloc::DMAAlloc::FreeDMABuffer(buf);
            }
        }

        if(!buf){
            Printk::Write(Printk::Level::LOG_ERR, " GPT: AHCI::ReadSectors returned NULL buffer for partition entries.\n");
            return FALSE;
        }

        GptPartitionEntry *entries = (GptPartitionEntry*)buf->VirtAddr;
        int PartitionCount = 0;

        for(U32 i = 0; i < GPTHeader0->NumberOfPartitionEntries; i++){
            GptPartitionEntry* entry = &entries[i];

            if (entry->PartitionTypeGUID.data1 == 0 && entry->PartitionTypeGUID.data2 == 0 &&
                entry->PartitionTypeGUID.data3 == 0 && entry->PartitionTypeGUID.data4[0] == 0) {
                continue; // Lewati entri kosong ini
            }

            PartitionCount++;
            Partition *newPartition = new Partition(entry, Device);
            if(newPartition){
                // Daftarkan partisi baru ke PartitionManager
                PartitionManager::RegisterPartition(newPartition, Device);
            } else {
                Printk::Write(Printk::Level::LOG_ERR, " GPT: Failed to allocate memory for new partition object.\n");
            }
        }
        PageAlloc::DMAAlloc::FreeDMABuffer(buf);
        return TRUE;
    }

    BOOL InitFs(){

        U32 DeviceCount = DeviceManager::GetBlockDeviceCount();
        if(DeviceCount == 0){
            Printk::Write(Printk::Level::LOG_ERR, " GPT: No block devices found in DeviceManager.\n");
            return FALSE;
        }

        //Printk::Write(Printk::Level::LOG_INFO, " GPT: Found %u block devices to scan.\n", DeviceCount);
        __MAYBE_UNUSED U8 AttemptGPTCall = 0;

        for(U32 i = 0 ; i < DeviceCount; i++){
            IBlockDevice *Device = DeviceManager::GetBlockDevice(i);
            if(!Device){
                Printk::Write(Printk::Level::LOG_WARNING, " GPT: Skipping NULL block device at index %u.\n", i);
                continue;
            }

            //Printk::Write(Printk::Level::LOG_DEBUG, " GPT: Initializing device %u...\n", i);

            GPTFS::GPTHeader *GPTHeader0 = nullptr;
            if(!InitializeGPT(Device, &GPTHeader0)){
                Printk::Write(Printk::Level::LOG_WARNING, " GPT: Device %u is not GPT formatted or failed to read GPT header.\n", i);
                //Printk::Write(Printk::Level::LOG_DEBUG, " GPT: Device name skipped is %s.\n", Device->GetDeviceName());
                continue;
            }

            if(!ParsePartitionEntries(Device, GPTHeader0)){
                Printk::Write(Printk::Level::LOG_ERR, " GPT: Failed to parse partition entries for device %u.\n", i);
                continue;
            }

            AttemptGPTCall++;
        }

        PartitionManager::InitializeRegisteredPartitionToFS();

        return TRUE;
    }

    BOOL ScanAllDevices() {
        U32 DeviceCount = DeviceManager::GetBlockDeviceCount();
        if(DeviceCount == 0) return FALSE;

        for(U32 i = 0 ; i < DeviceCount; i++){
            IBlockDevice *Device = DeviceManager::GetBlockDevice(i);
            if(!Device) continue;

            GPTFS::GPTHeader *GPTHeader0 = nullptr;
            if(InitializeGPT(Device, &GPTHeader0)){
                ParsePartitionEntries(Device, GPTHeader0);
            }
        }
        //PartitionManager::InitializeRegisteredPartitionToFS();
        return TRUE;
    }
}