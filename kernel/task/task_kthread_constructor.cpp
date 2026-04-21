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

    Task *ConstructTask(VOID (*Entry)(VOID *Context1), VOID* ContextArg, const char *taskname){
        Task *NewTask = new Task();
        if(NewTask == nullptr){
            Printk::Write(Printk::Level::LOG_ERR, "ConstructTask: failed to allocate Task struct\n");
            return nullptr;
        }
        String::Memset(NewTask, 0, sizeof(Task));

        NewTask->StackSize = 0x4000; // 16 KB stack
        NewTask->StackBase = (VOID*)Kmalloc::Alloc(NewTask->StackSize);
        if(NewTask->StackBase == nullptr){
            Printk::Write(Printk::Level::LOG_ERR, "ConstructTask: failed to allocate stack for Task\n");
            delete NewTask;
            return nullptr;
        }

        NewTask->pid = (U64)-1; // will be set by SchedulerAddTask
        String::Strcpy(NewTask->Name, taskname);
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
        Frame->rdi = (U64)ContextArg; // First argument in RDI

        NewTask->RSP = FrameAddr;

        NewTask->State = TaskState::READY;
        NewTask->vruntime = MinVRuntime;
        NewTask->NiceValueOfThisGuy = 0;
        NewTask->Weight = RateThisTaskNice(NewTask->NiceValueOfThisGuy);
        NewTask->IsCriticalProc = FALSE;
        NewTask->IsSudoOrAdmin = FALSE;
        NewTask->IsEssentialSystem = FALSE;
        NewTask->RbtLeft = nullptr;
        NewTask->RbtRight = nullptr;
        NewTask->RbtParent = nullptr;
        NewTask->Color = RBT_RED;
        NewTask->YieldRequested = FALSE;

        for(U32 i = 0; i < MAX_FILE_IN_PROCESS; i++){
            NewTask->FDTable[i] = nullptr;
        }
        NewTask->MMapNextAddr = 0;
        UPTR Address = (UPTR)NewTask->FPU_Storage;
        
        // Geser ke alamat kelipatan 16 terdekat (Rounding Up)
        // Rumus: (Addr + 15) & ~15
        NewTask->FPU_Region = (U8*)((Address + 15) & ~0xF);

        // Debug: Pastikan alignment benar
        // Printk::Write(Printk::Level::LOG_DEBUG, "Task FPU Aligned at: %llx\n", (U64)NewTask->FPU_Region);
        
        // Sekarang aman, karena FPU_Region pasti aligned 16-byte
        Arch::ASM::FPU_Save(NewTask->FPU_Region);
        return NewTask;
    }

    VOID SchedulerAddTask(Task *NewTask){
        // AMANKAN DARI INTERRUPT SEKARANG JUGA
        LOCKRFLAGS rflags = Arch::SaveAndDisableInterrupts(); 

        // KASUS 1: Reserved PID
        if(NewTask->pid != (U64)-1) {
            if(NewTask->pid < MAX_TASK && TaskArray[NewTask->pid] == nullptr) {
                TaskArray[NewTask->pid] = NewTask;
                
                if(NewTask->pid != Tasking::PID_IDLE) Tasking::ActiveTask++;
                
                if (NewTask->State == TaskState::READY) {
                    Printk::Write(Printk::Level::LOG_DEBUG, "Enqueueing PID %u.\n", NewTask->pid);
                    
                    // Enqueue biasanya aman dipanggil saat int disabled (karena dia handle lock sendiri/nested)
                    // Tapi lebih baik cek implementasi Enqueue lu. 
                    // Kalau Enqueue melakukan locking lagi, pastikan SaveAndDisableInterrupts support nesting.
                    CFSEnqueue(NewTask); 
                }
            }
            Arch::RestoreInterrupts(rflags); // RESTORE
            return;
        }

        // KASUS 2: Auto-Assign
        for(U64 i = Tasking::PID_INIT; i < MAX_TASK; i++){
            if(TaskArray[i] == nullptr){
                NewTask->pid = i;
                
                if (NewTask->PGID == 0) NewTask->PGID = i; 

                // DISINI TITIK KRISISMU SEBELUMNYA
                TaskArray[i] = NewTask; 
                Tasking::ActiveTask++;

                if (NewTask->State == TaskState::READY) {
                    Printk::Write(Printk::Level::LOG_CRIT, "TaskConstructor: Enqueueing task PID %d.\n", NewTask->pid);
                    CFSEnqueue(NewTask);
                }
                
                Arch::RestoreInterrupts(rflags); // RESTORE & SELESAI
                return;
            }
        }
        
        Arch::RestoreInterrupts(rflags); // RESTORE KALAU GAGAL
        Printk::Write(Printk::Level::LOG_ERR, "Scheduler: Process Table Full! Cannot spawn PID >= 100\n");
    }

    Task* CreateKThread(VOID (*Entry)(VOID *Context), VOID *ContextArg, const char *taskname){
        Task *NewKThread = ConstructTask(Entry, ContextArg, taskname);
        if(NewKThread == nullptr){
            Printk::Write(Printk::Level::LOG_ERR, "Failed to create Kernel Thread\n");
            return nullptr;
        }
 
        SchedulerAddTask(NewKThread);

        return NewKThread;
    }

    VOID CreateIdleTask(VOID (*Entry)(VOID *ctx)){
        Task *Idle = ConstructTask(Entry, 0, "IdleTask");
        if(!Idle) return;
        
        // Paksa taruh di slot 0
        Idle->pid = PID_IDLE;
        String::Strcpy(Idle->Name, "System Idle");
        
        if(TaskArray[PID_IDLE] == nullptr){
            TaskArray[PID_IDLE] = Idle;
        } else {
             Printk::Write(Printk::Level::LOG_ERR, "PANIC: PID 0 already occupied!\n");
        }
    }
}