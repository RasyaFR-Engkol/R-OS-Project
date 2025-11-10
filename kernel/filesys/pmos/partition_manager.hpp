#pragma once

#include <rosval.h>

class Partition;

typedef enum {
    PARTITIONS_ERROR = -1,
    NO_PARTITIONS = 0,
    PARTITIONS_INITIALIZED = 1,
} PARTMANAGER;

namespace PartitionManager{
    void InitializePM();

    BOOL RegisterPartition(Partition *Part);

    Partition* GetPartitionByIndex(U32 Index);

    U32 GetPartitionCount();

    PARTMANAGER InitializeRegisteredPartitionToFS();
}