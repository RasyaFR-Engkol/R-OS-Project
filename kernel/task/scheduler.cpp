#include <rossys.hpp>
#define PRINTK_MODULE_NAME "Sched"
#include <logging.hpp>
#include <rosval.h>
#include "serial.hpp"
#include "task.hpp"
#include "../mm/mm.hpp"
#include <cpu_context.hpp>
#include "../../firmware/acpi/madt/madt.hpp"

extern "C" void Arch_SwitchStackAndResume(void* new_sp, void* resume_ip);
extern "C" void Arch_SwitchStackAndCall(void* new_sp, void (*target)(void*), void* arg);
extern "C" void Scheduler_IretTrampoline();
ABI_C VOID Context_Restore(VOID *Context);

namespace Tasking {
    VOID SchedulerStart() {
        // Find first task in TaskArray
        for (U64 i = 0; i < MAX_TASK; ++i) {
            Task *t = TaskArray[i];
            if (t == nullptr) continue;
            if (t->State != TaskState::READY && t->State != TaskState::RUNNING) continue;

            // Set current indices
            CurrentTaskIndex = i;

            // atur task ke state running karena ini running of course
            t->State = TaskState::RUNNING;

            // Load CR3 for the task if different
            if ((U64)KernelPML4Phys != t->CR3) {
                DoCR3::Load((uint64_t*)t->CR3);
            } 


            // Resume via iret using saved CpuContext_T memory pointed by RSP
            if (t->RSP == 0) {
                Printk::Write(Printk::Level::LOG_ERR,
                    "SchedulerStart: task PID %u has no saved RSP, skipping\n", (U64)i);
                continue;
            }

            SIZE_T rip_offset = offsetof(CpuContext_T, rip);
            U64 saved_rip = *((U64*)((UPTR)t->RSP + rip_offset));

            if (saved_rip == 0) {
                Printk::Write(Printk::Level::LOG_ERR,
                    "SchedulerStart: task PID %u saved RIP==0, skipping\n", (U64)i);
                continue;
            }

            // For kernel threads we must restore the full CpuContext_T so that
            // the saved RSP/SS are applied. Using an iretq directly when
            // returning to the same privilege level will only pop RIP/CS/RFLAGS
            // (not RSP/SS), which leaves the stack pointer incorrect and
            // corrupts later IRQ/context saves. Use Context_Restore via the
            // Arch_SwitchStackAndCall trampoline so the full register block
            // and iretq path are executed with the proper stack.

            Arch_SwitchStackAndCall((void*)t->RSP, (void (*)(void*))Context_Restore, (void*)t->RSP);

            // Should not return here; if it does, continue searching
        }

        // No runnable tasks found
        Printk::Write(Printk::Level::LOG_ERR, "SchedulerStart: no runnable tasks\n");
    }

    // Variable Global untuk tracking booster
    static U64 GlobalTickCounter = 0;

    VOID SchedulerTick(void *context){
        CpuContext_T *Context = (CpuContext_T*)context;
        Task *PrevTask = TaskArray[CurrentTaskIndex];

        if(PrevTask != nullptr){
            PrevTask->RSP = (U64)Context;
        }

        BOOL IsYield = FALSE;

        if (PrevTask && PrevTask->YieldRequested) {
            IsYield = TRUE;
            PrevTask->YieldRequested = FALSE; // Reset flag
        }

        if(PrevTask != nullptr && PrevTask->State == TaskState::RUNNING){
            PrevTask->TimeSlice--;
            PrevTask->TimeUsedInPriority++;

            U64 Allotment = GetTimeAllotmentForPriority(PrevTask->Priority);

            if(PrevTask->TimeUsedInPriority >= Allotment){
                if(PrevTask->Priority < MLFQ_LEVELS - 1){
                    PrevTask->Priority++;
                }

                PrevTask->TimeUsedInPriority = 0;
                PrevTask->TimeSlice = GetTimeSliceForPriority(PrevTask->Priority);
                PrevTask->State = TaskState::READY;
            } else if(PrevTask->TimeSlice == 0){
                PrevTask->TimeSlice = GetTimeSliceForPriority(PrevTask->Priority);
                PrevTask->State = TaskState::READY;
            } else if(!IsYield && PrevTask->TimeSlice > 0 && PrevTask->State == TaskState::RUNNING) {
                return;
            } else if(IsYield){
                PrevTask->State = TaskState::READY;
            }
        }

        GlobalTickCounter++;
        if(GlobalTickCounter >= PRIORITY_BOOST_INTERVAL){
            GlobalTickCounter = 0;
            for(VAL32 i = 0; i < MAX_TASK; i++){
                if(TaskArray[i]){
                    TaskArray[i]->Priority = 0;
                    TaskArray[i]->TimeUsedInPriority = 0;
                    TaskArray[i]->TimeSlice = GetTimeSliceForPriority(0);
                }
            }
        }

        Task *NextTask = nullptr;

        for(U8 p = 0; p < MLFQ_LEVELS; p++){
            U64 ScanIndex = CurrentTaskIndex + 1;
            for(U64 i = 0; i < MAX_TASK; i++){
                if(ScanIndex >= MAX_TASK) ScanIndex = 0;

                Task *t = TaskArray[ScanIndex];

                if(t != nullptr && t->State == TaskState::READY && t->Priority == p){
                    NextTask = t;
                    CurrentTaskIndex = ScanIndex;
                    goto FoundTask;
                }

                ScanIndex++;
            }
        }

        FoundTask:{
            if(NextTask != nullptr){
                NextTask->State = TaskState::RUNNING;

                U64 CurrentCR3 = (U64)DoCR3::GetCurrentCR3();
                if(NextTask->CR3 != CurrentCR3){
                    DoCR3::Load((U64*)NextTask->CR3);
                }

                Context_Restore((VOID*)NextTask->RSP);
            }

            // disini ksoong? kita panic
            Printk::Write(Printk::Level::LOG_EMERG, ": PANIC : PID 0 and 1 not running\n");
        }
    }

    U64 GetTimeSliceForPriority(U8 Priority){
        // Simple mapping: higher priority (lower number) gets more time slice
        switch(Priority){
            case 0: return 5; // Highest priority
            case 1: return 10;
            case 2: return 20;
            default: return 40; // Lowest priority
        }
    }

    U64 GetTimeAllotmentForPriority(U8 priority) {
        switch(priority) {
            case 0: return 20;  // Boleh jalan 4x (kalau slice=5) sebelum turun
            case 1: return 40;
            case 2: return 80;
            default: return 0xFFFFFFFFFFFFFFFF; // Prio terendah stay forever
        }
    }

    VOID SchedulerYield(){
        UNUSED__ Task *Current = TaskArray[CurrentTaskIndex];

        // FOR DEBUGGING PURPOSES
        //Printk::Write(Printk::Level::LOG_DEBUG, "SchedulerYield: Task PID %u yielding CPU\n", (U64)Current->pid);

        // sebenarnya ini langsung aja yield bisa.
        // tapi emang gini doang implementasinya anjir?
        // gw gak sure cok. sesimple ini? oh ya ya

        Current->YieldRequested = TRUE;

        Arch::ASM::Interrupt(CONFIG_TIMER_HEXA_GLOBAL);
    }
}