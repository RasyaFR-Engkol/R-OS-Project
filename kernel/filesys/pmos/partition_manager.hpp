#pragma once

#include <rosval.h>

class Partition;

typedef enum {
    PARTITIONS_ERROR = -1,
    NO_PARTITIONS = 0,
    PARTITIONS_INITIALIZED = 1,
} PARTMANAGER;

#define PARTITIONMANAGERFUNC

namespace PartitionManager{
    void InitializePM();

    BOOL RegisterPartition(Partition *Part);

    Partition* GetPartitionByIndex(U32 Index);

    U32 GetPartitionCount();

    PARTMANAGER InitializeRegisteredPartitionToFS();

    // membuat struct untuk mengembalikan data yang diminta oleh
    // userland untuk mengetahui Partisi yang ada di sistem
    typedef enum _PARTITION_STATE{
        IS_UNMOUNTED = -1,
        IS_READONLY = 0,
        IS_READWRITE = 1, // mounted ini berarti
        IS_BOOTABLE = 2
    } PARTITION_STATE;

    typedef struct __PARTITION_M{
        CHAR8 PartitionName[64];
        CHAR8 MountPoint[128];
        CHAR8 FSType[16];      // <--- Tambahin ini (e.g., "FAT32", "EXT2")
        U64 TotalBytes;
        U64 UsedBytes;         // <--- Biar bisa bikin ProgressBar kapasitas di UI
        U32 BlockSize;
        PARTITION_STATE State;
    } PARTITION_DATA_SYSTEM;
}