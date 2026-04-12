#include "string.hpp"
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
#include <spinlock/simple.hpp>

extern "C" void Arch_SwitchStackAndResume(void* new_sp, void* resume_ip);
extern "C" void Arch_SwitchStackAndCall(void* new_sp, void (*target)(void*), void* arg);
extern "C" void Scheduler_IretTrampoline();
ABI_C VOID Context_Restore(VOID *Context);

static U64 LastSchedulerTickTimestamp = 0;
VOLATILE U64 GlobalBoostEpoch = 0;
static VOLATILE U64 TotalSleepingTasks = 0; // Tambah ini
VOLATILE U32 PriorityBitmap = 0;

namespace Tasking {
    RunQueue PriorityQueues[MLFQ_LEVELS];
    static Arch::Spinlock::Spinlock RunqueueLock;
    Task *SleepingHead = nullptr;
    Task* QueueHead[MLFQ_LEVELS] = { nullptr };
    Task* QueueTail[MLFQ_LEVELS] = { nullptr };
    Task* SleepWheel[TIMER_WHEEL_SIZE] = { nullptr };

    VOID Enqueue(Task *t){
        Arch::Spinlock::SpinlockGuard Guard(RunqueueLock);
        
        if(!t) {
            Printk::Write(Printk::Level::LOG_DEBUG, "No Task to Enqueue.\n");
            return;
        }

        if (t->NextRunQueue != nullptr || t->PrevRunQueue != nullptr) {
            return;
        }

        if(t->Priority >= MLFQ_LEVELS) t->Priority = MLFQ_LEVELS - 1;

        if (t->LastBoostEpoch < GlobalBoostEpoch) {
            // MASA BOOST: Angkat SEMUA task ke Priority 0 biar kebagian CPU!
            // Jangan khawatir, task biasa nanti otomatis turun kasta lagi di SchedulerTick.
            t->Priority = 0; 
            
            t->TimeSlice = GetTimeSliceForPriority(t->Priority);
            t->LastBoostEpoch = GlobalBoostEpoch;
        }

        RunQueue &Rq = PriorityQueues[t->Priority];

        t->NextRunQueue = nullptr;
        t->PrevRunQueue = Rq.Tail;

        if(Rq.Tail) Rq.Tail->NextRunQueue = t;
        else Rq.Head = t;

        Rq.Tail = t;
        Rq.Count++;

        PriorityBitmap |= (1 << t->Priority);
    }

    VOID InternalEnqueue(Task *t){        
        if(!t) {
            Printk::Write(Printk::Level::LOG_DEBUG, "No Task to Enqueue.\n");
            return;
        }

        if (t->NextRunQueue != nullptr || t->PrevRunQueue != nullptr) {
            return;
        }

        if(t->Priority >= MLFQ_LEVELS) t->Priority = MLFQ_LEVELS - 1;

        if (t->LastBoostEpoch < GlobalBoostEpoch) {
            // MASA BOOST: Angkat SEMUA task ke Priority 0 biar kebagian CPU!
            // Jangan khawatir, task biasa nanti otomatis turun kasta lagi di SchedulerTick.
            t->Priority = 0; 
            
            t->TimeSlice = GetTimeSliceForPriority(t->Priority);
            t->LastBoostEpoch = GlobalBoostEpoch;
        }

        RunQueue &Rq = PriorityQueues[t->Priority];

        t->NextRunQueue = nullptr;
        t->PrevRunQueue = Rq.Tail;

        if(Rq.Tail) Rq.Tail->NextRunQueue = t;
        else Rq.Head = t;

        Rq.Tail = t;
        Rq.Count++;

        PriorityBitmap |= (1 << t->Priority);
    }

    Task *Dequeue(){
         Arch::Spinlock::SpinlockGuard Guard(RunqueueLock);

        if (PriorityBitmap == 0) {
            return nullptr;
        }

        INTN HighestPrio = __builtin_ctz(PriorityBitmap);
        RunQueue &Rq = PriorityQueues[HighestPrio];

        Task *t = Rq.Head;
        if(!t) {
            return nullptr;
        }

        Rq.Head = t->NextRunQueue;
        if(Rq.Head) Rq.Head->PrevRunQueue = nullptr;
        else {
            Rq.Tail = nullptr;
            PriorityBitmap &= ~(1 << HighestPrio);
        }

        Rq.Count--;
        t->NextRunQueue = nullptr;
        t->PrevRunQueue = nullptr;

        return t;
    }

    VOID AddToSleepList(Task *t) {
        LOCKRFLAGS rflags = Arch::SaveAndDisableInterrupts();
        
        // Hitung index bucket (Hashing sederhana)
        U64 index = t->SleepTick % TIMER_WHEEL_SIZE;
        
        // Insert di Head (O(1))
        t->NextSleepQueue = SleepWheel[index];
        SleepWheel[index] = t;
        TotalSleepingTasks = TotalSleepingTasks + 1;
        
        Arch::RestoreInterrupts(rflags);
    }

    // Variable static untuk track tick terakhir yang SUDAH dicek
    static VOLATILE U64 LastCheckedTick = 0; 

    VOID CheckSleepingTasks() {
        if (TotalSleepingTasks == 0) {
            // Kalau gak ada yg tidur, sync aja biar gak loop jauh nanti
            LastCheckedTick = ACPI::Timer::LapicTicks; 
            return;
        }

        U64 Now = ACPI::Timer::LapicTicks;
        
        // Safety: Kalau waktu mundur (bug timer) atau sama, skip
        if (Now <= LastCheckedTick) return;

        // --- CATCH UP LOGIC ---
        // Kita harus iterasi dari LastCheckedTick + 1 sampai Now
        // Biar bucket yang ke-skip (misal tick 662) tetap diperiksa.
        
        U64 StartTick = LastCheckedTick + 1;
        
        // Optimasi: Kalau gap-nya lebih besar dari ukuran wheel,
        // Cukup scan 1 putaran penuh wheel aja. Gak usah loop 1 juta kali.
        if ((Now - LastCheckedTick) > TIMER_WHEEL_SIZE) {
            StartTick = Now - TIMER_WHEEL_SIZE + 1;
        }

        LOCKRFLAGS rflags = Arch::SaveAndDisableInterrupts();

        // Loop range tick yang terlewat
        for (U64 currTick = StartTick; currTick <= Now; currTick++) {
            U64 index = currTick % TIMER_WHEEL_SIZE;
            
            Task* t = SleepWheel[index];
            Task* prev = nullptr;

            while (t) {
                // Perhatikan: Bucket ini mungkin isi task masa depan (hash collision)
                // Jadi tetap harus cek t->SleepTick <= Now
                if (Now >= t->SleepTick) {
                    // WAKEUP!
                    Task* next = t->NextSleepQueue;
                    TotalSleepingTasks = TotalSleepingTasks - 1;

                    // Unlink
                    if (prev) prev->NextSleepQueue = next;
                    else SleepWheel[index] = next;

                    t->NextSleepQueue = nullptr;
                    t->State = TaskState::READY;
                    
                    // Reset data sleep biar bersih di dump
                    t->SleepTick = 0;
                    
                    Enqueue(t);
                    t = next;
                } else {
                    prev = t;
                    t = t->NextSleepQueue;
                }
            }
        }
        
        Arch::RestoreInterrupts(rflags);
        
        // Update tracker
        LastCheckedTick = Now;
    }

    VOID Sleep(U64 ms) {
        Task *Current = GetCurrentTaskPtr();
        if (!Current || ms == 0) return;

        // Hitung target tick berdasarkan frekuensi LAPIC yang sudah dikalibrasi
        U64 TicksToSleep = (ms * ACPI::Timer::LapicHz) / 1000;
        if (TicksToSleep == 0) TicksToSleep = 1;

        Current->SleepTick = ACPI::Timer::LapicTicks + TicksToSleep;

        // TANDAI: Task ini BLOCKED dan jangan dimasukkan ke RunQueue (Enqueue)
        Current->State = Tasking::TaskState::BLOCKED;
        Current->BlockReason |= TASK_SLEEPING;

        // Masukkan ke list tidur (SleepingHead) yang akan dicek di tiap tick
        AddToSleepList(Current);

        // Panggil scheduler untuk memilih task lain
        SchedulerYield();
    }

    U64 GetDistToNextWakeup(U64 Now){
        U64 SearchLimit = 100;
        for(U64 i = 0; i < SearchLimit; i++){
            U64 TargetTick = Now + i;
            U64 Index = TargetTick % TIMER_WHEEL_SIZE;

            Task *t = SleepWheel[Index];
            while(t){
                if(t->SleepTick <= TargetTick){
                    return i;
                }
                t  = t->NextSleepQueue;
            }
        }
        return 0xFFFFFFFFFFFFFFFF;
    }
        
    VOID SchedulerStart() {
        Arch::ASM::Cli();
        SchedulerActive = FALSE;

        for (U64 i = 1; i < MAX_TASK; ++i) { 
            Task *t = TaskArray[i];
            if (t && t->State == TaskState::READY) {
                Enqueue(t);
            }
        }

        Task *First = Dequeue();

        if(First){
            CurrentTaskIndex = First->pid;
            First->State = TaskState::RUNNING;

            U64 kernelCr3 = (U64)KernelPML4Phys;
            if (kernelCr3 != First->CR3) {
                DoCR3::Load((uint64_t*)First->CR3);
            }
            TSS::SetRsp0((UPTR)First->StackBase + First->StackSize);
            
            SchedulerActive = TRUE;
            Arch_SwitchStackAndCall((void*)First->RSP, (void (*)(void*))Context_Restore, (void*)First->RSP);
        }

        Task *Idle = TaskArray[PID_IDLE];
        if (Idle) {
            CurrentTaskIndex = PID_IDLE;
            Idle->State = TaskState::RUNNING;
            SchedulerActive = TRUE;
            Arch_SwitchStackAndCall((void*)Idle->RSP, (void (*)(void*))Context_Restore, (void*)Idle->RSP);
        }

        while(1) Arch::ASM::HaltCPU();
    }

    // Variable Global untuk tracking booster
    static U64 GlobalTickCounter = 0;
    VOID DestroyTask(Task *task);

    VOID SchedulerTick(void *context){
        // 1. Lock biar aman (Opsional kalau single core, tapi good practice)
        Arch::ASM::Cli(); 
        
        CpuContext_T *Context = (CpuContext_T*)context;
        if(!SchedulerActive) return;    

        Task *PrevTask = nullptr;
        if(CurrentTaskIndex < MAX_TASK){
            PrevTask = TaskArray[CurrentTaskIndex];
        }

        // 2. Simpan Context Task Lama (PrevTask)
        if(PrevTask != nullptr){
            PrevTask->RSP = (U64)Context;
            if (PrevTask->FPU_Region) {
                Arch::ASM::FPU_Save(PrevTask->FPU_Region);
            }
        }

        // Update System Tick
        U64 CurrentSystemTick = ACPI::Timer::LapicTicks;
        U64 DeltaTicks = 1; 
        if (LastSchedulerTickTimestamp != 0) {
            DeltaTicks = CurrentSystemTick - LastSchedulerTickTimestamp;
        }
        LastSchedulerTickTimestamp = CurrentSystemTick;

        // 3. LOGIKA PREV TASK (YANG WAJIB ADA)
        if(PrevTask && PrevTask->pid != PID_IDLE){
            // Handle Yield manual / Force Reschedule
            if(Tasking::ForceReschedule || PrevTask->YieldRequested){
                Tasking::ForceReschedule = FALSE;
                PrevTask->YieldRequested = FALSE;
                
                PrevTask->TimeUsedInPriority = 0;

                if(PrevTask->IsCriticalProc || PrevTask->IsEssentialSystem){
                    PrevTask->Priority = 0;
                    PrevTask->TimeSlice = GetTimeSliceForPriority(0);
                }
            } 
            else {
                // Logic MLFQ TimeSlice (Pengurangan jatah waktu)
                if(PrevTask->TimeSlice >= DeltaTicks) PrevTask->TimeSlice -= DeltaTicks;
                else PrevTask->TimeSlice = 0;

                PrevTask->TimeUsedInPriority += DeltaTicks;

                if(PrevTask->IsCriticalProc || PrevTask->IsEssentialSystem){
                    // Set allotment tinggi buat VVIP (misal 1000ms alias 1 detik)
                    U64 CriticalAllotment = 1000; 

                    if (PrevTask->TimeUsedInPriority >= CriticalAllotment) {
                        // Kalau VVIP rakus banget, hukum turun ke Prio 1 sementara
                        PrevTask->Priority = 1; 
                        PrevTask->TimeUsedInPriority = 0;
                        PrevTask->TimeSlice = GetTimeSliceForPriority(1);
                    } else if (PrevTask->TimeSlice == 0) {
                        PrevTask->Priority = 0; // Tetap di Prio 0
                        PrevTask->TimeSlice = GetTimeSliceForPriority(0);
                    }
                } else {
                    U64 Allotment = GetTimeAllotmentForPriority(PrevTask->Priority);

                    if(PrevTask->TimeUsedInPriority >= Allotment){
                        if(PrevTask->Priority < MLFQ_LEVELS - 1) PrevTask->Priority++;
                        PrevTask->TimeUsedInPriority = 0;
                        PrevTask->TimeSlice = GetTimeSliceForPriority(PrevTask->Priority);
                    }
                    else if(PrevTask->TimeSlice == 0){
                        PrevTask->TimeSlice = GetTimeSliceForPriority(PrevTask->Priority);
                    }
                }
            }
            if (PrevTask->State == TaskState::RUNNING || PrevTask->State == TaskState::READY) {
                PrevTask->State = TaskState::READY;
                
                // Masukin lagi ke antrian biar Scheduler tau dia masih mau jalan
                Enqueue(PrevTask); 
            }
        }
        
        // 4. Bangunin Task yang lagi tidur (Sleep)
        CheckSleepingTasks();

        // 5. Global Boost (Anti Starvation)
        GlobalTickCounter += DeltaTicks;
        if(GlobalTickCounter >= PRIORITY_BOOST_INTERVAL){
            GlobalTickCounter = 0;
            GlobalBoostEpoch = GlobalBoostEpoch + 1;

            Arch::Spinlock::SpinlockGuard Guard(RunqueueLock);

            for(int i = 1; i < MLFQ_LEVELS; i++){
                if((PriorityBitmap & (1 << i)) != 0) {

                    Task *Current = PriorityQueues[i].Head; 
                    while(Current != nullptr){
                        Task *NextTask = Current->NextRunQueue;

                        Current->Priority = 0;
                        Current->TimeUsedInPriority = 0;
                        Current->TimeSlice = GetTimeSliceForPriority(0);

                        InternalEnqueue(Current);

                        Current = NextTask;
                    }

                    PriorityQueues[i].Head = nullptr;
                    PriorityQueues[i].Tail = nullptr;
                    PriorityQueues[i].Count = 0;
                    
                    PriorityBitmap &= ~(1 << i);
                }
            }
        }

        // 6. Ambil Task Baru (Dequeue)
        Task *NextTask = Dequeue();

        // --- JEBAKAN BATMAN (Panic Check) ---
            // Kalau masih null, pake IDLE
            if (NextTask == nullptr) {
                NextTask = TaskArray[PID_IDLE];
            }
        // ------------------------------------

        if(NextTask == nullptr){
            NextTask = TaskArray[PID_IDLE];
        }

        // 7. Context Switch ke NextTask
        UNUSED__ ExecuteSwitch:{
            if(NextTask != nullptr){
                CurrentTaskIndex = NextTask->pid;

                // Hitung Timer Interrupt selanjutnya
                U64 TimeSliceInTSC = NextTask->TimeSlice * ACPI::Timer::TscTicksPerSystemTick;
                const U64 MIN_SYSTEM_TICK_MS = 4;
                U64 SystemTickInTSC = MIN_SYSTEM_TICK_MS * ACPI::Timer::TscTicksPerSystemTick; 
                
                if (TimeSliceInTSC > SystemTickInTSC) TimeSliceInTSC = SystemTickInTSC;
                if (TimeSliceInTSC < 5000) TimeSliceInTSC = 5000;

                ACPI::Timer::Arm(TimeSliceInTSC);
                
                // Restore State
                NextTask->State = TaskState::RUNNING;

                U64 CurrentCR3 = (U64)DoCR3::GetCurrentCR3();
                if(NextTask->CR3 != CurrentCR3){
                    DoCR3::Load((U64*)NextTask->CR3);
                }

                if (NextTask->FPU_Region) {
                    Arch::ASM::FPU_Restore(NextTask->FPU_Region);
                }

                TSS::SetRsp0((UPTR)NextTask->StackBase + NextTask->StackSize);
                Context_Restore((VOID*)NextTask->RSP);
            } else {
                Printk::Panic("No init task. SYSBRK.\n");
            }
        }
    }

    U64 GetTimeSliceForPriority(U8 Priority){
        // Angka dalam satuan Tick (ms)
        switch(Priority){
            case 0: return 20;  
            case 1: return 40;  
            case 2: return 80; 
            default: return 100; 
        }
    }

    U64 GetTimeAllotmentForPriority(U8 priority) {
        switch(priority) {
            case 0: return 400; // Setelah 100ms total CPU time, turun kasta
            case 1: return 800;
            case 2: return 1200;
            default: return 0xFFFFFFFFFFFFFFFF; 
        }
    }

    VOID SchedulerYield(){
        Arch::ASM::Sti(); // Pastikan interrupt nyala
        
        // Panggil Scheduler
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

    VOID SleepOn(WaitQueue &Queue){
        Arch::ASM::Cli();

        Task *Current = GetCurrentTaskPtr();
        if(!Current){
            Arch::ASM::Sti();
            return;
        }

        if (Current->pid == 101 || Current->pid == 102) { 
            Printk::Write(Printk::Level::LOG_DEBUG, 
                "SNIPER: PID %d BLOCKED (SleepOn) at Queue Addr %p\n", 
                Current->pid, &Queue);
        }

        Current->State = TaskState::BLOCKED;

        Current->NextWaitTask = nullptr;

        if(Queue.Head == nullptr){
            Queue.Head = Current;
            Queue.Tail = Current;
        } else {
            Queue.Tail->NextWaitTask = Current;
            Queue.Tail = Current;
        }

        ForceReschedule = TRUE;

        Arch::ASM::Sti();

        SchedulerYield();
    }

    VOID WakeUp(WaitQueue &queue) {
        // 1. Cek ada yang tidur gak?
        if (queue.Head == nullptr) return;

        // 2. Ambil task paling depan (Compositor lu biasanya disini)
        Task *t = queue.Head;
        
        // 3. Majuin Head ke task berikutnya (Pake NextWaitTask!)
        queue.Head = t->NextWaitTask;
        
        // Kalau antrian jadi kosong, update Tail juga
        if (queue.Head == nullptr) {
            queue.Tail = nullptr;
        }

        // Putus link task ini dari antrian lama biar bersih
        t->NextWaitTask = nullptr;

        // 4. Set status jadi READY dan masukin ke RunQueue Scheduler
        t->State = TaskState::READY;
        t->BlockReason = 0; // Clear reason
        
        // 5. Preempt! Kalau yang bangun (Prio 0) lebih penting dari yang jalan sekarang (Prio 3)
        // Tendang yang sekarang, kasih CPU ke yang baru bangun.
        Task* Current = GetCurrentTaskPtr();
        if (Current && t->Priority < Current->Priority) {
            ForceReschedule = TRUE;
        }

        UnblockTaskWithIOBoost(t); 
    }
    
    VOID Debug_DumpProcessState() {
        Printk::Write(Printk::Level::LOG_INFO, "\n=== SCHEDULER DUMP ===\n");
        U64 Now = ACPI::Timer::LapicTicks;
        
        // Masukkan PID yang mau dipantau disini
        // Misal: 1=Init, 99=Compositor, 100=AnyApp
        U64 TargetPIDs[] = {0, 1, 2, 3, 4}; 
        
        for (int i = 0; i <= 4; i++) {
            Task* t = Tasking::GetTaskPID(TargetPIDs[i]);
            if (!t) continue;

            const char* stateStr = "UNKNOWN";
            switch(t->State) {
                case TaskState::RUNNING: stateStr = "RUNNING"; break;
                case TaskState::READY:   stateStr = "READY"; break;
                case TaskState::BLOCKED: stateStr = "BLOCKED"; break;
                case TaskState::ZOMBIE:  stateStr = "ZOMBIE"; break;
                case TaskState::TERMINATED: stateStr = "TERMINATED"; break;
            }

            Printk::Write(Printk::Level::LOG_INFO, 
                "PID: %d | State: %s | Prio: %d | Slice: %d\n",
                t->pid, stateStr, t->Priority, t->TimeSlice
            );

            Printk::Write(Printk::LOG_INFO, "Is Essential: %s | Is Critical: %s | Is Sudo/Admin: %s\n",
                t->IsEssentialSystem ? "YES" : "NO",
                t->IsCriticalProc ? "YES" : "NO",
                t->IsSudoOrAdmin ? "YES" : "NO"
            );

            Printk::Write(Printk::Level::LOG_DEBUG, "PriorityBitmap: 0x%x | ActiveTask: %d\n", PriorityBitmap, ActiveTask);

            if (t->State == TaskState::BLOCKED) {
                Printk::Write(Printk::Level::LOG_INFO, 
                    "   -> BLOCKED. SleepTick: %d (Delta: %d)\n", 
                    t->SleepTick, (U64)(t->SleepTick - Now)
                );
            }
        }
        Printk::Write(Printk::Level::LOG_INFO, "======================\n");
    }

    VOID Debug_DumpFDProccessBelowPID10(){
        for(INTN i = 1; i < 10; i++){
            Task *Ptr = GetTaskPID(i);
            if(!Ptr) continue;
            Printk::Write(Printk::Level::LOG_DINFO, "FD in PID %d.\n", i);

            for(INTN in = 0; in < MAX_FILE_IN_PROCESS; in++){
                File *f = Ptr->FDTable[in];
                if(!f) continue;

                CHAR8 FileName[64];
                String::Memset(FileName, 0, sizeof(FileName));

                String::Strcpy(FileName, f->FileName);

                Printk::Write(Printk::Level::LOG_DINFO,
                            "FD[%u]: Name: %s: Type: %d: RefCount:%d.\n", in, FileName, f->type, f->RefCount);
            }
        }
    }

    VOID Debug_MinorAndMajorFaultsBelowPID10(){
        for(INTN i = 1; i <= 10; i++){
            Task *Tasking = GetTaskPID(i);
            if(!Tasking) continue;

            U64 Major = Tasking->CountMajorFault;
            U64 Minor = Tasking->CountMinorFault;

            Printk::Write(Printk::Level::LOG_DEBUG, "PID %d Fault.\n", i);
            Printk::Write(Printk::Level::LOG_DEBUG, "MAJOR = %u.\n", Major);
            Printk::Write(Printk::Level::LOG_DEBUG, "MINOR = %u.\n", Minor);
        }
    }
}