#include "../partition_manager.hpp"
#include <rosval.h>
#define PRINTK_MODULE_NAME "PManager"
#include <logging.hpp>
#include "../../../usrlndtrnstn/syscall/syscall/sysarg.hpp"
#include <mm.hpp>
#include "../../partition.hpp"

namespace PartitionManager{
    namespace ReservedSyscall{
        U32 GetPartitionMuch(){ // SYSCALL NUMBER 2000 (2000 reserved keatas untuk partition manager)
            return GetPartitionCount();
        }
        
        VOID PARTITIONMANAGERFUNC GetPartition(CpuContext_T *ctx){
            U64 Index = CATCHARG1(ctx);
            //PARTITION_DATA_SYSTEM *Address = (PARTITION_DATA_SYSTEM*)CATCHARG2(ctx);

            Tasking::Task *Curtask = Tasking::GetCurrentTaskPtr();
            if(!Curtask){
                RETVAL(ctx) = -ROS_ERROR_NO_ENTRY;
                return;
            }

            //U64 UserPML4 = (U64)HHDM_PhysToVirt(Curtask->CR3);
            
            Partition *part = GetPartitionByIndex(Index);
            if(!part) {
                RETVAL(ctx) = -ROS_ERROR_NO_ENTRY;
                return;
            }

            PARTITION_DATA_SYSTEM KData;
            String::Memset(&KData, 0, sizeof(PARTITION_DATA_SYSTEM));

            String::Strcpy(KData.PartitionName, "Disk Part");
            KData.BlockSize = part->GetSectorCount() * 512;
            String::Strcpy(KData.FSType, "UNKNOWN");
            //String::Strcpy(KData.MountPoint, part->)
        }
    }
}