#include <rossys.hpp>
#include <rosval.h>
#include <userland/syscall.hpp>
#include "cpu_context.hpp"
#include "string.hpp"
#include "task.hpp"
// need paging helpers
#include "../mm/mm.hpp"
#include <string.hpp>
#include <mm.hpp>
#include <logging.hpp>  

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

    // Helper to clone a page table level
    // Returns Physical Address of the new table
    static UPTR CloneTable(UPTR SrcPhys, int Level) {
        UPTR NewPhys = PageAlloc::PhysicalAllocPages(1);
        if (!NewPhys) return 0;

        U64* SrcVirt = HHDM_PhysToVirt(SrcPhys);
        U64* NewVirt = HHDM_PhysToVirt(NewPhys);
        String::Memset(NewVirt, 0, PAGE_SIZE);

        for (int i = 0; i < 512; ++i) {
            if (!(SrcVirt[i] & PAGE_PRESENT)) continue;

            if (Level > 1) {
                // Directory (PML4, PDPT, PD)
                // Recurse
                UPTR ChildSrcPhys = SrcVirt[i] & PAGE_ADDR_MASK;
                UPTR ChildNewPhys = CloneTable(ChildSrcPhys, Level - 1);
                if (!ChildNewPhys) return 0; // TODO: Cleanup

                // Copy flags but point to new child
                NewVirt[i] = ChildNewPhys | (SrcVirt[i] & ~PAGE_ADDR_MASK);
            } else {
                // Page Table (Level 1)
                // Allocate new physical page for data
                UPTR PageSrcPhys = SrcVirt[i] & PAGE_ADDR_MASK;
                UPTR PageNewPhys = PageAlloc::PhysicalAllocPages(1);
                if (!PageNewPhys) return 0; // TODO: Cleanup

                // Copy data
                void* PageSrcVirt = (void*)HHDM_PhysToVirt(PageSrcPhys);
                void* PageNewVirt = (void*)HHDM_PhysToVirt(PageNewPhys);
                String::Memcpy(PageNewVirt, PageSrcVirt, PAGE_SIZE);

                // Map new page
                NewVirt[i] = PageNewPhys | (SrcVirt[i] & ~PAGE_ADDR_MASK);
            }
        }
        return NewPhys;
    }

    U64 CloneUserAddressSpace(U64 SourceCR3) {
        // 1. Create Base (Kernel Mappings)
        U64 NewCR3 = CreateUserAddressSpace();
        if (!NewCR3) return 0;

        U64* SrcPML4 = HHDM_PhysToVirt(SourceCR3);
        U64* NewPML4 = HHDM_PhysToVirt(NewCR3);

        // 2. Clone User Mappings (0-255)
        for (int i = 0; i < 256; ++i) {
            if (SrcPML4[i] & PAGE_PRESENT) {
                UPTR ChildSrcPhys = SrcPML4[i] & PAGE_ADDR_MASK;
                UPTR ChildNewPhys = CloneTable(ChildSrcPhys, 3); // Start at Level 3 (PDPT)
                if (!ChildNewPhys) return 0; // TODO: Cleanup
                NewPML4[i] = ChildNewPhys | (SrcPML4[i] & ~PAGE_ADDR_MASK);
            }
        }
        return NewCR3;
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

        NewTask->pid = (U64)-1; // will be set by SchedulerAddTask
        String::Strcpy(NewTask->Name, "KThread");
        NewTask->NextTask = nullptr;

        NewTask->CR3 = KernelPML4Phys; // kernel thread uses kernel page tables

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
        // KASUS 1: PID sudah di-request secara spesifik (Reserved)
        // Contoh: Idle Task (0), Input Daemon (1)
        if(NewTask->pid != (U64)-1) {
            if(NewTask->pid < MAX_TASK && TaskArray[NewTask->pid] == nullptr) {
                TaskArray[NewTask->pid] = NewTask;
                // Jangan increment ActiveTask kalau Idle (opsional, tergantung logic load balancer)
                if(NewTask->pid != Tasking::PID_IDLE) Tasking::ActiveTask++;
                Printk::Write(Printk::Level::LOG_INFO, "Scheduler: Reserved Process Spawned PID %d\n", NewTask->pid);
            } else {
                Printk::Write(Printk::Level::LOG_ERR, "SchedulerAddTask: Slot %d busy/invalid for reserved task!\n", NewTask->pid);
            }
            return;
        }

        // KASUS 2: PID Auto-Assign (User Tasks & Generic KThreads)
        // Kita cari slot kosong mulai dari PID_USER_START (100)
        // Biar slot 0-99 aman bersih buat Kernel Services.
        for(U64 i = Tasking::PID_USER_START; i < MAX_TASK; i++){
            if(TaskArray[i] == nullptr){
                NewTask->pid = i;
                
                // Kalau PGID belum di-set, jadikan dia leader grup diri sendiri
                if (NewTask->PGID == 0) NewTask->PGID = i; 

                TaskArray[i] = NewTask;
                Tasking::ActiveTask++;
                
                // Debug log
                Printk::Write(Printk::Level::LOG_DEBUG, "Scheduler: New Process Spawned PID %d\n", i);
                return;
            }
        }
        
        Printk::Write(Printk::Level::LOG_ERR, "Scheduler: Process Table Full! Cannot spawn PID >= 100\n");
        // TODO: Handle failure (delete NewTask & free memory)
    }

    VOID CreateKThread(VOID (*Entry)(VOID)){
        Task *NewKThread = ConstructTask(Entry);
        if(NewKThread == nullptr){
            Printk::Write(Printk::Level::LOG_ERR, "Failed to create Kernel Thread\n");
            return; 
        }
 
        SchedulerAddTask(NewKThread);
    }

    VOID CreateIdleTask(VOID (*Entry)(VOID)){
        Task *Idle = ConstructTask(Entry);
        if(!Idle) return;
        
        // Paksa taruh di slot 0
        Idle->pid = PID_IDLE;
        Idle->Priority = MLFQ_LEVELS - 1; // Prioritas paling rendah
        String::Strcpy(Idle->Name, "System Idle");
        
        if(TaskArray[PID_IDLE] == nullptr){
            TaskArray[PID_IDLE] = Idle;
        } else {
             Printk::Write(Printk::Level::LOG_ERR, "PANIC: PID 0 already occupied!\n");
        }
    }
}