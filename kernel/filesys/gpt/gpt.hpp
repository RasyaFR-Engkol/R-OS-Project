#pragma once

#include "rosval.h"
#include "../iblockdevice.hpp"

namespace GPTFS{
    struct GPTGuid{
        U32 data1;
        U16 data2;
        U16 data3;
        U8  data4[8];
    } PACKSTRUCT;

    struct GPTHeader{
        U64 Signature;
        U32 Revision;
        U32 HeaderSize;
        U32 HeaderCRC32;
        U32 Reserved;
        U64 MyLBA;
        U64 BackupLBA;
        U64 FirstUsableLBA;
        U64 LastUsableLBA;
        GPTGuid DiskGUID;
        U64 PartitionEntryLBA;
        U32 NumberOfPartitionEntries;
        U32 SizeOfPartitionEntry;
        U32 PartitionEntryArrayCRC32;
        VOID *Reserved2;
    } PACKSTRUCT;

    struct GptPartitionEntry {
        GPTGuid PartitionTypeGUID;    // 0x00 (16 bytes) - GUID Tipe Partisi
        GPTGuid UniquePartitionGUID;  // 0x10 (16 bytes) - GUID Unik Partisi
        U64     StartingLBA;          // 0x20 (8 bytes)  - LBA Awal
        U64     EndingLBA;            // 0x28 (8 bytes)  - LBA Akhir (inklusif)
        U64     Attributes;           // 0x30 (8 bytes)  - Atribut partisi
        U16     PartitionName[36];    // 0x38 (72 bytes) - Nama partisi (UTF-16LE)
    } PACKSTRUCT;

    struct PartitionInfo{
        U64 StartingLBA;
        U64 SectorCount;
        GPTGuid PartitionTypeGUID;
        GPTGuid UniquePartitionGUID;
        U16  PartitionName[36];
    };

    BOOL InitializeGPT(IBlockDevice *Device, GPTFS::GPTHeader **OutHeader);

    BOOL ParsePartitionEntries(IBlockDevice* Device, GPTFS::GPTHeader* GPTHeader0);

    BOOL InitFs();
}