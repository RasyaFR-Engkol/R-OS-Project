#include <rossys.hpp>
#define PRINTK_MODULE_NAME "Sched"
#include <logging.hpp>
#include <rosval.h>
#include "serial.hpp"
#include "task.hpp"
#include "../mm/mm.hpp"
#include <cpu_context.hpp>
#include "../../firmware/acpi/madt/madt.hpp"
#include "../../x86_64/tss.hpp"
#include <rostime.hpp>
#include <../firmware/acpi/driver/timer/timer.hpp>

extern "C" void Arch_SwitchStackAndResume(void* new_sp, void* resume_ip);
extern "C" void Arch_SwitchStackAndCall(void* new_sp, void (*target)(void*), void* arg);
extern "C" void Scheduler_IretTrampoline();
ABI_C VOID Context_Restore(VOID *Context);

static U64 LastSchedulerTickTimestamp = 0;

namespace Tasking {
    VOID SchedulerStart() {
        Arch::ASM::Cli();
        SchedulerActive = FALSE;

        // [MODIFIED] Search start from PID 1 (Input Daemon) or User Process
        // Kita skip PID 0 (Idle) sebisa mungkin di awal start.
        for (U64 i = 1; i < MAX_TASK; ++i) { 
            Task *t = TaskArray[i];
            if (t == nullptr) continue;
            if (t->State != TaskState::READY && t->State != TaskState::RUNNING) continue;

            CurrentTaskIndex = i;
            t->State = TaskState::RUNNING;

            U64 kernelCr3 = (U64)KernelPML4Phys;
            if (kernelCr3 != t->CR3) {
                DoCR3::Load((uint64_t*)t->CR3);
            }

            TSS::SetRsp0((UPTR)t->StackBase + t->StackSize);

            if (t->RSP == 0) continue;

            // Restore logic...
            SchedulerActive = TRUE;
            Arch_SwitchStackAndCall((void*)t->RSP, (void (*)(void*))Context_Restore, (void*)t->RSP);
        }

        // Kalau tidak ada task lain (misal Input Daemon gagal init),
        // Fallback ke Idle Task (PID 0)
        Task *Idle = TaskArray[PID_IDLE];
        if (Idle && Idle->State == TaskState::READY) {
            Printk::Write(Printk::Level::LOG_NOTICE, "SchedulerStart: Starting directly into Idle Task\n");
            CurrentTaskIndex = PID_IDLE;
            Idle->State = TaskState::RUNNING;
            SchedulerActive = TRUE;
            Arch_SwitchStackAndCall((void*)Idle->RSP, (void (*)(void*))Context_Restore, (void*)Idle->RSP);
        }

        Printk::Write(Printk::Level::LOG_ERR, "SchedulerStart: HALT (No Tasks)\n");
        while(1) Arch::ASM::HaltCPU();
    }

    // Variable Global untuk tracking booster
    static U64 GlobalTickCounter = 0;

    VOID DestroyTask(Task *task);

    VOID SchedulerTick(void *context){
        CpuContext_T *Context = (CpuContext_T*)context;
        if(!SchedulerActive) return;    

        Task *PrevTask = nullptr;
        if(CurrentTaskIndex < MAX_TASK){
            PrevTask = TaskArray[CurrentTaskIndex];
        }

        if(PrevTask != nullptr){
            PrevTask->RSP = (U64)Context;
        }

        U64 CurrentSystemTick = ACPI::Timer::LapicTicks;

        U64 DeltaTicks = 1; 
        if (LastSchedulerTickTimestamp != 0) {
            DeltaTicks = CurrentSystemTick - LastSchedulerTickTimestamp;
        }
        // Safety: minimal 1 biar logika jalan
        if (DeltaTicks == 0) DeltaTicks = 1;

        LastSchedulerTickTimestamp = CurrentSystemTick;

        // Check pending signals for the currently running task (PrevTask)
        // Handle SIGINT immediately so a running foreground process is
        // stopped as soon as possible (fix race where signal was set while
        // the task stayed RUNNING until a later tick)
        // Only handle SIGINT (signal number 2) here.
        if(PrevTask && PrevTask->pid >= PID_USER_START){
            if (PrevTask->Signals & (1 << 2)) {
                if(PrevTask->pid == PID_USER_START){
                    Printk::Write(Printk::Level::LOG_EMERG, "Scheduler: Init killed\n");
                }
                PrevTask->Signals &= ~(1 << 2); // clear signal flag

                // Mark as ZOMBIE so it's not scheduled again
                PrevTask->State = TaskState::ZOMBIE;

                // Wake parent if it's waiting
                U64 ppid = PrevTask->ppid;
                if (ppid < MAX_TASK) {
                    Task *parentTask = TaskArray[ppid];
                    if (parentTask != nullptr && parentTask->State == TaskState::BLOCKED) {
                        parentTask->State = TaskState::READY;
                    }
                }

                // Nothing more to do for this task; continue scheduling other tasks
                // Do not attempt to destroy the task here (unsafe in tick).
                PrevTask = nullptr;

                Printk::Write(Printk::Level::LOG_DEBUG, "Scheduler: Task PID %llu received SIGINT and marked as ZOMBIE\n", PrevTask->pid);

                
            }
        }

        BOOL IsYield = FALSE;

        if (PrevTask && PrevTask->YieldRequested) {
            IsYield = TRUE;
            PrevTask->YieldRequested = FALSE; // Reset flag
        }

        if(PrevTask != nullptr && PrevTask->State == TaskState::RUNNING){
            if(PrevTask->pid == PID_IDLE || PrevTask->pid == PID_INPUT){
                PrevTask->State = TaskState::READY;
            } else {
                if (PrevTask->TimeSlice >= DeltaTicks) {
                    PrevTask->TimeSlice -= DeltaTicks;
                } else {
                    PrevTask->TimeSlice = 0;
                }

                PrevTask->TimeUsedInPriority += DeltaTicks; // Tambah sebesar Delta

                U64 Allotment = GetTimeAllotmentForPriority(PrevTask->Priority);

                if(PrevTask->TimeUsedInPriority >= Allotment){
                    if(PrevTask->Priority < MLFQ_LEVELS - 1)PrevTask->Priority++;
                    PrevTask->TimeUsedInPriority = 0;
                    PrevTask->TimeSlice = GetTimeSliceForPriority(PrevTask->Priority);
                    PrevTask->State = TaskState::READY;
                } else if(PrevTask->TimeSlice == 0){
                    PrevTask->TimeSlice = GetTimeSliceForPriority(PrevTask->Priority);
                    PrevTask->State = TaskState::READY;
                } else if(!IsYield && PrevTask->TimeSlice > 0 && PrevTask->State == TaskState::RUNNING) {
                    BOOL HigherPriorityWaiting = FALSE;

                    if(Tasking::ForceReschedule){
                        Tasking::ForceReschedule = FALSE;
                        HigherPriorityWaiting = TRUE;
                    } else {
                        for(U64 i = 0; i < MAX_TASK; i++){
                            Task *t = TaskArray[i];
                            if(!t || t->State != TaskState::READY) continue;

                            if(t->Priority < PrevTask->Priority){
                                HigherPriorityWaiting = TRUE;
                                break;
                            }
                        }
                    }

                    if(!HigherPriorityWaiting){
                        return;
                    } else {
                        PrevTask->State = TaskState::READY;
                    }
                } else if(IsYield){
                    PrevTask->State = TaskState::READY;
                }
            }
        }

        GlobalTickCounter++;
        if(GlobalTickCounter >= PRIORITY_BOOST_INTERVAL){
            GlobalTickCounter = 0;
            for(U64 i = 0; i < MAX_TASK; i++){
                Task* t = TaskArray[i];
                if(t && t->State != TaskState::ZOMBIE && t->pid != PID_IDLE){
                    t->Priority = 0;
                    t->TimeUsedInPriority = 0;
                    t->TimeSlice = GetTimeSliceForPriority(0);
                }
            }
        }

        Task *NextTask = nullptr;

        for(U8 p = 0; p < MLFQ_LEVELS; p++){
            U64 ScanIndex = CurrentTaskIndex + 1;
            for(U64 i = 0; i < MAX_TASK; i++){
                if(ScanIndex >= MAX_TASK) ScanIndex = 0;

                if(ScanIndex == PID_IDLE) {
                    ScanIndex++;
                    continue;
                }

                Task *t = TaskArray[ScanIndex];

                if(t != nullptr && t->State == TaskState::READY && t->Priority == p){
                    NextTask = t;
                    CurrentTaskIndex = ScanIndex;
                    goto FoundTask;
                } else if(t != nullptr && t->State == TaskState::BLOCKED && (t->BlockReason & TASK_SLEEPING)){
                    if(CurrentSystemTick >= t->SleepTick) { // t->SleepTick disini adalah Target Bangun
                        t->State = TaskState::READY;
                        t->BlockReason &= ~TASK_SLEEPING;   
                        t->SleepTick = 0; // Reset
                        NextTask = t;
                        CurrentTaskIndex = ScanIndex;
                        goto FoundTask;
                    }
                }

                ScanIndex++;
            }
        }

        FoundTask:{
            if(NextTask != nullptr){
// --- TICKLESS LOGIC: CALCULATE NEXT DEADLINE ---
                
                // 1. Kapan timeslice task ini habis?
                U64 TimeSliceInTSC = NextTask->TimeSlice * ACPI::Timer::TscTicksPerSystemTick;
                
                // 2. Kapan task tidur terdekat harus bangun?
                U64 TimeUntilNearestWakeup = 0xFFFFFFFFFFFFFFFF;

                for(U64 i=0; i<MAX_TASK; ++i) {
                    Task* t = TaskArray[i];
                    if(t && t->State == TaskState::BLOCKED && (t->BlockReason & TASK_SLEEPING)) {
                        U64 dist = 0;
                        if (t->SleepTick > CurrentSystemTick) {
                            dist = t->SleepTick - CurrentSystemTick;
                        } else {
                            dist = 1; // Harusnya udah bangun, trigger ASAP
                        }
                        
                        U64 wakeDistTSC = dist * ACPI::Timer::TscTicksPerSystemTick;
                        if(wakeDistTSC < TimeUntilNearestWakeup) {
                            TimeUntilNearestWakeup = wakeDistTSC;
                        }
                    }
                }

                // Pilih mana yang lebih cepat: Timeslice habis ATAU Task lain bangun
                U64 NextInterruptDelta = TimeSliceInTSC;
                if (TimeUntilNearestWakeup < NextInterruptDelta) {
                    NextInterruptDelta = TimeUntilNearestWakeup;
                }

                // Minimum safety margin buat TSC Deadline (biar gak hang kalau delta kedekitan)
                if (NextInterruptDelta < 2000) NextInterruptDelta = 2000;

                // Arm Timer!
                ACPI::Timer::Arm(NextInterruptDelta);

                // --- CONTEXT SWITCH ---
                NextTask->State = TaskState::RUNNING;

                U64 CurrentCR3 = (U64)DoCR3::GetCurrentCR3();
                // Hanya ganti CR3 kalau beda (optimasi TLB flush)
                if(NextTask->CR3 != CurrentCR3){
                    DoCR3::Load((U64*)NextTask->CR3);
                }

                // Update TSS RSP0 agar kalau ada interrupt di Ring 3, CPU tau stack kernel di mana
                TSS::SetRsp0((UPTR)NextTask->StackBase + NextTask->StackSize);

                // Load Register Task Baru
                Context_Restore((VOID*)NextTask->RSP);
            } else {
                U64 TicksToSleep = 100;

                for(U64 i=0; i<MAX_TASK; ++i) {
                    Task* t = TaskArray[i];
                    if(t && t->State == TaskState::BLOCKED && (t->BlockReason & TASK_SLEEPING)) {
                        if (t->SleepTick > CurrentSystemTick) {
                            U64 dist = t->SleepTick - CurrentSystemTick;
                            if(dist < TicksToSleep) TicksToSleep = dist;
                        } else {
                            // Ada task yang harusnya udah bangun! Jangan tidur lama-lama.
                            TicksToSleep = 1; 
                        }
                    }
                }

                if(TicksToSleep < 1) TicksToSleep = 1;
                U64 TscDelta = TicksToSleep * ACPI::Timer::TscTicksPerSystemTick;

                ACPI::Timer::Arm(TscDelta);

                Task *Idle = TaskArray[PID_IDLE];
                if(Idle != nullptr){
                    //Printk::Write(Printk::Level::LOG_DEBUG, "CPU Idle..\n");
                    Idle->State = TaskState::RUNNING;
                    CurrentTaskIndex = PID_IDLE; // Set index ke 0
                    
                    // Idle task selalu di kernel space, tidak perlu ganti CR3 biasanya
                    // kecuali idle task punya page table sendiri (jarang).
                    // Tapi TSS RSP0 tetap perlu update jaga-jaga ada interrupt.
                    TSS::SetRsp0((UPTR)Idle->StackBase + Idle->StackSize);
                    
                    Context_Restore((VOID*)Idle->RSP);
                } else {
                    Printk::Write(Printk::Level::LOG_EMERG, "PANIC: Idle Task Died/Missing!\n");
                    while(1) Arch::ASM::HaltCPU();
                }
            }
        }
    }

    U64 GetTimeSliceForPriority(U8 Priority){
        // Angka dalam satuan Tick (ms)
        switch(Priority){
            case 0: return 10;  // 10ms (Sangat responsif)
            case 1: return 40;  // 40ms
            case 2: return 100; // 100ms
            default: return 200; // Background tasks dikasih jatah panjang biar jarang context switch
        }
    }

    U64 GetTimeAllotmentForPriority(U8 priority) {
        switch(priority) {
            case 0: return 100; // Setelah 100ms total CPU time, turun kasta
            case 1: return 500;
            case 2: return 1000;
            default: return 0xFFFFFFFFFFFFFFFF; 
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

    // Return a copy of the currently running Task struct.
    Task GetCurrentTask(){
        if(CurrentTaskIndex < MAX_TASK){
            Task *t = TaskArray[CurrentTaskIndex];
            if(t != nullptr){
                return *t; // return copy
            }
        }
        Task empty{};
        empty.pid = 0;
        return empty;
    }
    
    Task* GetCurrentTaskPtr(){
        if(CurrentTaskIndex < MAX_TASK){
            return TaskArray[CurrentTaskIndex];
        }
        return nullptr;
    }
}