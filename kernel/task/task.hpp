#pragma once

// task.hpp
// 
// Ini untuk task scheduler pertama kita yang berbasis
// round-robin. Task scheduler ini akan mengatur penjadwalan
// task-task yang berjalan di sistem operasi kita.
//
// Note: Ini Task Control Block (TCB) yang sangat sederhana.
// Di implementasi nyata, TCB biasanya memiliki banyak
// informasi tambahan seperti prioritas, status task, dan
// informasi konteks CPU lainnya.

#include "rosval.h"
#include <cpu_context.hpp>
// Forward-declare File to avoid heavy include and circular dependencies
class File;

#define MAX_FILE_IN_PROCESS 16
#define MLFQ_LEVELS 4
#define PRIORITY_BOOST_INTERVAL 2000

// Ini harusnya adjustable terhadap kebutuhan sistem operasi kita
// tapi untuk sekarang kita tetapkan 256 Task saja.
#define MAX_TASK 256

// FLAGS MLFQ
#define TASK_SLEEPING (1 << 0)

namespace Tasking {
    constexpr U64 PID_IDLE = 0;
    constexpr U64 PID_INPUT = 1;
    constexpr U64 PID_DISK = 2; // Reserved masa depan
    constexpr U64 PID_REAPD = 3; // Reserved masa depan
    constexpr U64 DEFAULT_CONFIG_PID_START = 100; // Default user process start PID
    constexpr U64 PID_USER_START = 100; // User process mulai dari sini
}

namespace Tasking{
    struct TaskState{
        enum State{
            READY,
            RUNNING,
            BLOCKED,
            TERMINATED,
            ZOMBIE
        };
    };

    struct Task{
        U64 pid; // Process ID
        U64 ppid; // Parent Process ID
        U64 PGID; // Process Group ID
        Task *PGIDTaskPtr; // Pointer ke task pertama di grup ini
        U32 Signals; // Pending Signals
        U64 SignalHandlers[32]; // Signal Handlers (0 = Default/Terminate)
        CHAR8 Name[32]; // Task Name

        U64 RSP; // Pointer ke konteks CpuContext_T yang siap di-irestore
        U64 CR3; // Page Table Base Register (untuk virtual memory)

        VOID* StackBase; // Pointer ke base stack (HHDM virtual)
        U64 StackSize;

        U8 Priority; // Prioritas task (0 = tertinggi)
        U64 TimeUsedInPriority;

        Tasking::TaskState::State State; // Current State of the Task
        UFLAGS BlockReason; 

        // untuk tidur berapa ya tick nya?
        U64 SleepTick;
        
        Tasking::Task *NextWaitTask;

        U64 TimeSlice; // Time slice for scheduling
        U64 SleepUntil; // Waktu hingga task ini harus dibangunkan (jika tidur)

        Tasking::Task *NextTask; // Pointer to the next task in the scheduler's list

        File *FDTable[MAX_FILE_IN_PROCESS]; // File Descriptor Table

        U64 MMapNextAddr; // Untuk syscall mmap, nyimpen alamat mmap berikutnya

        BOOL YieldRequested; // Tambah ini

        CHAR8 CWD[256];
    };

    // Variabl global untuk task management
    extern Task *TaskArray[MAX_TASK];
    extern Task *GraveyardArray[MAX_TASK];
    extern U64 ActiveTask;
    extern U64 CurrentTaskIndex;
    extern BOOL SchedulerActive;
    extern U64 g_ForegroundPID;
    extern BOOL ForceReschedule;

    VOID SchedulerStart();
    VOID CreateKThread(VOID (*Entry)(VOID));
    VOID CreateUserTask(const CHAR8 *Name, VOID *ELFImage);
    VOID SchedulerTick(void *context);
    U64 GetTimeSliceForPriority(U8 Priority);
    U64 GetTimeAllotmentForPriority(U8 priority);
    VOID SchedulerYield();
    U64 CreateUserAddressSpace();
    U64 CloneUserAddressSpace(U64 SourceCR3);
    VOID SchedulerAddTask(Task *NewTask);
    Task* TaskUserConstructor(const CHAR8 *Name, VOID *ELFImage);
    VOID DestroyTask(Task *task);
    VOID FreeUserAddressSpace(U64 cr3); // Expose this
    // Return a copy of the currently running Task struct. If no current
    // task exists, returns a zeroed Task with pid==0.
    Task GetCurrentTask();
    // Faster accessor returning pointer to the current Task (or nullptr)
    // Use this when you need to access fields like CR3 without copying.
    Task* GetCurrentTaskPtr();
    Task *GetTaskPID(U64 pid);
    Task *GetTaskPGID(U64 pid);
    // Set a signal on a task or a process group. If `isGroup` is TRUE,
    // `id` is interpreted as a PGID and the signal is delivered to all
    // members of that group. `signal` is the POSIX signal number
    // (e.g., 2 for SIGINT).
    VOID SetTaskSignal(U64 id, U32 signal, BOOL isGroup);
    VOID UnlinkFromProcGrp(Task *T);
    VOID UnblockTaskWithIOBoost(Task *T);
    VOID CreateIdleTask(VOID (*Entry)(VOID));

    // RESERVED TASK
    VOID InputDaemonTask();
    Task *ConstructTask(VOID (*Entry)(VOID));
    VOID ReapDTask();
}