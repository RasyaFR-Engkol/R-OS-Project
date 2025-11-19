#include <rossys.hpp>
#include <rosval.h>
#include <userland/syscall.hpp>
#include "cpu_context.hpp"
#include "string.hpp"
#include "task.hpp"
// need paging helpers
#include "../mm/mm.hpp"

namespace Tasking{
    U64 CreateUserAddressSpace(){
        // Setiap kembalian pasti akan mengembalikan CR3 kosong
        // dengan HHDM Kernel di atasnya
        // Allocate one physical page for new PML4
        UPTR new_pml4_phys = PageAlloc::PhysicalAllocPages(1);
        if (new_pml4_phys == 0) return 0; // allocation failed

        // Get HHDM (higher-half) virtual address to modify the page table
        U64 *NewPML4 = HHDM_PhysToVirt(new_pml4_phys);

        // Zero the new PML4
        for (size_t i = 0; i < 512; ++i) NewPML4[i] = 0;

        // Copy kernel higher-half entries so user space sees kernel mapped
        // Kernel uses indices 256..511 for higher-half alias; copy them
        for (size_t i = 256; i < 512; ++i) {
            NewPML4[i] = KernelPML4[i];
        }

        // Also ensure the HHDM mapping entry is present in the new PML4
        const SIZE_T HHDM_PML4_INDEX = (SIZE_T)((HHDM_BASE >> 39) & 0x1ff);
        NewPML4[HHDM_PML4_INDEX] = KernelPML4[HHDM_PML4_INDEX];

        // Return CR3 physical address (to be loaded into CR3 when switching)
        return (U64)new_pml4_phys;
    }

    Task *ConstructTask(VOID (*Entry)(VOID)){
        Task *NewTask = new Task();
        if(NewTask == nullptr){
            Printk::Write(Printk::Level::LOG_ERR, "ConstructTask: failed to allocate Task struct\n");
            return nullptr;
        }

        NewTask->StackSize = 0x4000; // 16 KB stack
        NewTask->StackBase = (VOID*)Kmalloc::Alloc(NewTask->StackSize);
        if(NewTask->StackBase == nullptr){
            Printk::Write(Printk::Level::LOG_ERR, "ConstructTask: failed to allocate stack for Task\n");
            delete NewTask;
            return nullptr;
        }

        NewTask->pid = 0; // will be set by SchedulerAddTask
        String::Strcpy(NewTask->Name, "KThread");
        NewTask->NextTask = nullptr;

        NewTask->CR3 = (U64)DoCR3::GetCurrentCR3();

        U64 StackTopAddr = (U64)NewTask->StackBase + NewTask->StackSize;
        U64 FrameAddr = StackTopAddr - sizeof(CpuContext_T);
        CpuContext_T *Frame = (CpuContext_T*)FrameAddr;

        String::Memset(Frame, 0, sizeof(CpuContext_T));

        #define KERNEL_DS (0x10)
        #define KERNEL_CS (0x08)
        Frame->rip = (U64)Entry;
        Frame->cs = KERNEL_CS;
        Frame->rflags = 0x202; // IF=1
        Frame->rsp = StackTopAddr;
        Frame->ss = KERNEL_DS;

        NewTask->RSP = FrameAddr;

        NewTask->State = TaskState::READY;
        NewTask->Priority = 0;
        NewTask->TimeSlice = 5;
        NewTask->SleepUntil = 0;

        for(U32 i = 0; i < MAX_FILE_IN_PROCESS; i++){
            NewTask->FDTable[i] = nullptr;
        }
        NewTask->MMapNextAddr = 0;

        return NewTask;
    }

    VOID SchedulerAddTask(Task *NewTask){
        // Cari slot kosong di TaskArray
        for(U64 i = 0; i < MAX_TASK; i++){
            if(TaskArray[i] == nullptr){
                NewTask->pid = i;
                TaskArray[i] = NewTask;
                // If this is the only task, make it loop to itself
                if(Tasking::ActiveTask == 0){
                    // Point to itself so scheduler loops when this is the only task
                    NewTask->NextTask = NewTask;
                }
                //Printk::Write(Printk::Level::LOG_INFO, "Added new Task with PID %u at slot %u\n", NewTask->pid, i);
                Tasking::ActiveTask++;
                return;
            }
        }
        //Printk::Write(Printk::Level::LOG_ERR, "Failed to add new Task: TaskArray full\n");
    }

    VOID CreateKThread(VOID (*Entry)(VOID)){
        Task *NewKThread = ConstructTask(Entry);
        if(NewKThread == nullptr){
            Printk::Write(Printk::Level::LOG_ERR, "Failed to create Kernel Thread\n");
            return;
        }

        SchedulerAddTask(NewKThread);
    }
}