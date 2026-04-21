#include "rbt.hpp"
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

static VOLATILE U64 TotalSleepingTasks = 0; // Tambah ini

namespace Tasking {
    Task *CFSRoot = nullptr;
    Task *CFSLeftmost = nullptr;
    Task *SleepWheel[TIMER_WHEEL_SIZE] = { nullptr };
    VOLATILE U64 MinVRuntime = 0;

    VOID CFSEnqueue(Task *NewTask){
        if (!NewTask || NewTask->pid == PID_IDLE) return;
        Task **Link = &CFSRoot;
        Task *Parent = nullptr;
        BOOL Leftmost = TRUE;

        while(*Link){
            Parent = *Link;
            /* Perbandingan kalo vruntime task lebih kecil daripada task di atas nya or parent */
            if(NewTask->vruntime < Parent->vruntime){
                Link = &Parent->RbtLeft; // Turun tingkat
            }
            /* Perbandingan kalo vruntime task lebih besar daripada task di atas nya or parent */
            else {
                Link = &Parent->RbtRight;
                Leftmost = FALSE;
            }
        }

        // Udah mentok sampe null.
        NewTask->RbtParent = Parent;
        NewTask->RbtLeft = nullptr;
        NewTask->RbtRight = nullptr;
        NewTask->Color = RBT_RED;
        *Link = NewTask;

        if(Leftmost){
            CFSLeftmost = NewTask;
        }

        RBT_InsertFixup(&CFSRoot, NewTask);

        Task* current = Tasking::GetCurrentTaskPtr();
        if (current && NewTask->vruntime < current->vruntime) {
            Tasking::ForceReschedule = TRUE;
        }
    }

    VOID CFSDequeue(Task *T){
        if (T == CFSLeftmost) {
            Task *NextLeft = T->RbtRight;
            if (NextLeft) {
                while (NextLeft->RbtLeft) NextLeft = NextLeft->RbtLeft;
                CFSLeftmost = NextLeft;
            } else {
                CFSLeftmost = T->RbtParent;
            }
        }
        // PERBAIKAN DI SINI: Root di depan, Task di belakang
        RBT_Erase(&CFSRoot, T); 
    }

    Task *CFSPickNext() {
        Task *Next = CFSLeftmost;

        if(Next){
            CFSDequeue(Next);
        }

        return Next;
    }

    VOID AddToSleepList(Task *t) {
        if (!t) return;

        LOCKRFLAGS rflags = Arch::SaveAndDisableInterrupts();

        // Sanity: if SleepTick is not set, nothing to add.
        if (t->SleepTick == 0) {
            Arch::RestoreInterrupts(rflags);
            return;
        }

        // Defensive: prevent duplicate insertion. Scan wheel buckets
        // under interrupt-disabled to ensure we don't create cycles.
        for (U64 i = 0; i < TIMER_WHEEL_SIZE; ++i) {
            Task *it = SleepWheel[i];
            while (it) {
                if (it == t) {
                    Arch::RestoreInterrupts(rflags);
                    return; // already in sleep wheel
                }
                it = it->NextSleepQueue;
            }
        }

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

                    if (MinVRuntime > 10) {
                        t->vruntime = MinVRuntime - 10;
                    } else {
                        t->vruntime = MinVRuntime;
                    }
                    
                    CFSEnqueue(t);
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

        // --- FOR LOOP YANG LAMA UDAH GW HAPUS BIAR GAK DOUBLE INSERT ---

        Task *First = CFSPickNext();

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
    UNUSED__ static U64 GlobalTickCounter = 0;
    VOID DestroyTask(Task *task);

    VOID Schedule(VOID *CurrentContextRSP) {
        Task *Prev = Tasking::GetCurrentTaskPtr();
        
        // 1. Simpan state dan HANYA masukkan kembali ke RBT jika dia masih RUNNING
        if (Prev) {
            // Selalu update RSP terakhir pas kena preempt
            Prev->RSP = (U64)CurrentContextRSP;
            
            // Selalu save FPU State kalau ada
            if (Prev->FPU_Region) {
                Arch::ASM::FPU_Save(Prev->FPU_Region);
            }

            // HANYA MASUKKAN KE RBT JIKA BUKAN IDLE TASK DAN MASIH RUNNING
            if (Prev->pid != PID_IDLE && Prev->State == TaskState::RUNNING) {
                Prev->State = TaskState::READY; 
                CFSEnqueue(Prev); 
            }
        }

        // 2. Ambil task yang paling butuh CPU
        Task *Next = CFSPickNext();
        if (!Next) {
            Next = GetTaskPID(PID_IDLE);
        } else {
            if (Next->vruntime > MinVRuntime) {
                MinVRuntime = Next->vruntime;
            }
        }

        // 3. Update pointer kernel
        Tasking::ActiveTask = Next->pid;
        CurrentTaskIndex = Next->pid; 
        Next->State = TaskState::RUNNING; 
        Tasking::ForceReschedule = FALSE;

        // ---> FIX: ARM TIMER LAPIC DI SINI BIAR GAK FREEZE <---
        // Kita set timer untuk meledak konstan setiap 4ms biar scheduler 
        // dapet kesempatan jalan lagi buat ngecek RBT (Preemption)
        const U64 MIN_SYSTEM_TICK_MS = 4;
        U64 SystemTickInTSC = MIN_SYSTEM_TICK_MS * ACPI::Timer::TscTicksPerSystemTick; 
        
        if (SystemTickInTSC < 5000) SystemTickInTSC = 5000; // Failsafe
        ACPI::Timer::Arm(SystemTickInTSC);
        // ------------------------------------------------------

        // 4. Switch CR3 (Page Table)
        U64 CurrentCR3 = (U64)DoCR3::GetCurrentCR3();
        if(Next->CR3 != CurrentCR3){
            DoCR3::Load((U64*)Next->CR3);
        }

        // 5. Restore FPU State
        if (Next->FPU_Region) {
            Arch::ASM::FPU_Restore(Next->FPU_Region);
        }

        // 6. Context Switch
        TSS::SetRsp0((UPTR)Next->StackBase + Next->StackSize);
        Context_Restore((VOID*)Next->RSP); 
    }

    VOID SchedulerTick(VOID *Context) {
        CheckSleepingTasks();

        Task *Current = Tasking::GetCurrentTaskPtr();
        
        if (Current && Current->pid != PID_IDLE) {
            // --- PRO-TIPS: CFS VRUNTIME CALCULATION ---
            // Rumus: vruntime += 1_tick * (NICE_0_WEIGHT / Current->Weight)
            // Kita pake skala (misal 1024) biar gak ilang presisinya pas pembagian.
            
            U64 delta_exec = 1; // 1 tick
            U64 weight_0 = 1024; // Weight untuk Nice 0
            
            // Hitung penalti vruntime
            // Task VIP (Weight > 1024) bakal punya 'penalty' < 1
            // Task Nyantai (Weight < 1024) bakal punya 'penalty' > 1
            U64 vdiff = (delta_exec * weight_0) / Current->Weight;
            
            // Minimal nambah 1 biar vruntime gak mandeg (stagnan)
            if (vdiff == 0) vdiff = 1;

            Current->vruntime += vdiff;
        }

        // Cek preemption
        if (CFSLeftmost) {
            if (Current && Current->pid == PID_IDLE) {
                ForceReschedule = TRUE;
            } else if (Current) {
                // --- OPTIMASI: Preemption Granularity ---
                // Jangan langsung switch cuma gara-gara beda 1 vruntime (bikin overhead)
                // Kasih napas dikit, misal beda 2-4 vruntime baru swap.
                U64 granularity = 2; 
                if (Current->vruntime > (CFSLeftmost->vruntime + granularity)) {
                    ForceReschedule = TRUE;
                }
            }
        }

        Schedule(Context);
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
        ACPI::Timer::Arm(1);
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
        if (queue.Head == nullptr) return;

        Task *t = queue.Head;
        queue.Head = t->NextWaitTask;
        
        if (queue.Head == nullptr) {
            queue.Tail = nullptr;
        }
        t->NextWaitTask = nullptr;

        t->State = TaskState::READY;
        t->BlockReason = 0; 
        
        // Langsung lempar ke fungsi unblock, dia yang bakal handle vruntime
        // dan set ForceReschedule = TRUE
        UnblockTaskWithIOBoost(t); 
    }
    
    VOID Debug_DumpProcessState() {
        Printk::Write(Printk::Level::LOG_INFO, "\n=== SCHEDULER DUMP ===\n");
        U64 Now = ACPI::Timer::LapicTicks;
        
        // Masukkan PID yang mau dipantau disini
        // Misal: 1=Init, 99=Compositor, 100=AnyApp
        U64 TargetPIDs[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}; 
        
        for (int i = 0; i <= 10; i++) {
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

            Printk::Write(Printk::LOG_INFO, "TASK NAME: %s.\n", t->Name);

            Printk::Write(Printk::Level::LOG_INFO, 
                "PID: %d | State: %s | Nice: %d | Weight: %u \n",
                t->pid, stateStr, t->NiceValueOfThisGuy, t->Weight
            );

            Printk::Write(Printk::LOG_INFO, "Is Essential: %s | Is Critical: %s | Is Sudo/Admin: %s\n",
                t->IsEssentialSystem ? "YES" : "NO",
                t->IsCriticalProc ? "YES" : "NO",
                t->IsSudoOrAdmin ? "YES" : "NO"
            );

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

    VOID Debug_PrintRBT(Task *node, INTN depth) {
        if (!node) return;

        // Print cabang Kanan (Di atas kalau diputar 90 derajat)
        Debug_PrintRBT(node->RbtRight, depth + 1);

        // Cetak indentasi (jarak) berdasarkan kedalaman
        for (INTN i = 0; i < depth; i++) {
            Printk::Write(Printk::Level::LOG_INFO, "        ");
        }

        // Cetak Warna, PID, dan vruntime
        const CHAR8* colorStr = (node->Color == RBT_RED) ? "[R]" : "[B]";
        Printk::Write(Printk::Level::LOG_INFO, "%s PID:%d(v:%llu)\n", 
                      colorStr, node->pid, node->vruntime);

        // Print cabang Kiri (Di bawah)
        Debug_PrintRBT(node->RbtLeft, depth + 1);
    }

    VOID DumpSchedulerTree() {
        Printk::Write(Printk::Level::LOG_INFO, "=== DUMP CFS RED-BLACK TREE ===\n");
        if (!CFSRoot) {
            Printk::Write(Printk::Level::LOG_INFO, "(Tree is empty)\n");
        } else {
            Debug_PrintRBT(CFSRoot, 0);
        }
        Printk::Write(Printk::Level::LOG_INFO, "===============================\n");
    }
}