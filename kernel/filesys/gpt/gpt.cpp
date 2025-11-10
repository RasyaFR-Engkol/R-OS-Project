#include "gpt.hpp"
#include <rosval.h>
#include <rossys.hpp>
#include <filesystem/filesystem.hpp>
#include <mm.hpp>
#define PRINTK_MODULE_NAME "GPTFS"
#include <logging.hpp>
#include <string.hpp>
#include "../pmos/partition_manager.hpp"
#include "../partition.hpp"
#include "../../mm/kmalloc/kmalloc.hpp"

namespace GPTFS{
    #define SECTOR_SIZE 512
    // kita perlu definisi dari GPTHeader
    GPTFS::GPTHeader *GPTHeader0 = nullptr;

    BOOL InitializeGPT(){
        // Kita bisa nyari AHCI Driver nya sendiri ajah
        // Kita coba baca LBA 1 (GPT Header)
        PageAlloc::DMAAlloc::DMABuffer *buf = nullptr;
        AHCI::AHCIPortInfo PortInfo = AHCI::GetPortInfo();
        if(PortInfo.port_number == (U8)-1){
            return FALSE;
        }

        AHCI::AHCIDriver AhciDRV = PortInfo.AhciDRV;
        VAL32 PortNum = PortInfo.port_number;

        if(!AHCI::ReadSectors(AhciDRV, PortNum, 1, 1, &buf)){
            return FALSE;
        }

        if(!buf){
            Printk::Write(Printk::Level::LOG_ERR, " GPT: AHCI::ReadSectors returned NULL buffer for header read.\n");
            return FALSE;
        }

        {
            // Free Buff, kita pake KMALLOC nanti
            GPTHeader0 = (GPTFS::GPTHeader*)Kmalloc::Alloc(sizeof(GPTFS::GPTHeader));
            if(!GPTHeader0){
                Printk::Write(Printk::Level::LOG_ERR, " GPT: Failed to allocate memory for GPT Header\n");
                PageAlloc::DMAAlloc::FreeDMABuffer(buf);
                return FALSE;
            }

            String::Memcpy((VOID*)GPTHeader0, (VOID*)buf->VirtAddr, sizeof(GPTFS::GPTHeader));
            // Kita bisa free DMA Buffer karena udah disalin
            PageAlloc::DMAAlloc::FreeDMABuffer(buf);
            // Dump GPT:
            Printk::Write(Printk::Level::LOG_INFO, " GPT Signature: 0x%llx\n", GPTHeader0->Signature);
            Printk::Write(Printk::Level::LOG_INFO, " Disk GUID: %08X-%04X-%04X-", 
                GPTHeader0->DiskGUID.data1, GPTHeader0->DiskGUID.data2, GPTHeader0->DiskGUID.data3);
            for(int i = 0; i < 8; i++){
                Printk::Write(Printk::Level::LOG_INFO, "%02X", GPTHeader0->DiskGUID.data4[i]);
                if(i == 1) Printk::Write(Printk::Level::LOG_INFO, "-");
            }
            Printk::Write(Printk::Level::LOG_INFO, "\n");

            return TRUE;
        }
    }
    
    BOOL ParsePartitionEntries(){
        if(!GPTHeader0){
            Printk::Write(Printk::Level::LOG_ERR, " GPT: GPT Header not initialized.\n");
            return FALSE;
        }

        AHCI::AHCIPortInfo PortInfo = AHCI::GetPortInfo();
        if(PortInfo.port_number == (U8)-1){
            Printk::Write(Printk::Level::LOG_ERR, " GPT: No active AHCI port found.\n");
            return FALSE;
        }

        Printk::Write(Printk::Level::LOG_INFO, "GPT: Parsing Partition Entries in LBA %llu\n", GPTHeader0->PartitionEntryLBA);

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
        if(!AHCI::ReadSectors(PortInfo.AhciDRV, PortInfo.port_number, GPTHeader0->PartitionEntryLBA, SectorsToRead, &buf)){
            if(buf){
                PageAlloc::DMAAlloc::FreeDMABuffer(buf);
            }
            Printk::Write(Printk::Level::LOG_ERR, " GPT: Failed to read Partition Entry Array\n");
            return FALSE;
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

            Printk::Write(Printk::Level::LOG_INFO, "  Partition No.%d:\n", PartitionCount);
            Printk::Write(Printk::Level::LOG_INFO, "    GUID Type: %08X-%04X-%04X-...\n", 
                entry->PartitionTypeGUID.data1, 
                entry->PartitionTypeGUID.data2, 
                entry->PartitionTypeGUID.data3);
            
            Printk::Write(Printk::Level::LOG_INFO, "    First LBA: %llu, Last LBA: %llu\n",
                entry->StartingLBA,
                entry->EndingLBA);

            Partition *NewPartition = new Partition(entry, PortInfo.AhciDRV, PortInfo.port_number);
            
            PartitionManager::RegisterPartition(NewPartition);
        }

        Printk::Write(Printk::Level::LOG_INFO, " GPT: Total %d partitions found.\n", PartitionCount);

        PageAlloc::DMAAlloc::FreeDMABuffer(buf);
        return TRUE;
    }

    BOOL InitFs(){
        PartitionManager::InitializePM();

        if(!InitializeGPT()){
            Printk::Write(Printk::Level::LOG_ERR, " GPT: Initialization failed.\n");
            return FALSE;
        }

        if(!ParsePartitionEntries()){
            Printk::Write(Printk::Level::LOG_ERR, " GPT: Failed to parse partition entries.\n");
            return FALSE;
        }

        PartitionManager::InitializeRegisteredPartitionToFS();

        return TRUE;
    }
}