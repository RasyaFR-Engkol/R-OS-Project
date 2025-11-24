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
#include <filesystem/filesystem.hpp>

#define MAX_FILE_IN_PROCESS 16
#define MLFQ_LEVELS 4
#define PRIORITY_BOOST_INTERVAL 2000

// Ini harusnya adjustable terhadap kebutuhan sistem operasi kita
// tapi untuk sekarang kita tetapkan 256 Task saja.
#define MAX_TASK 256

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
        U32 Signals; // Pending Signals
        CHAR8 Name[32]; // Task Name

        U64 RSP; // Pointer ke konteks CpuContext_T yang siap di-iret

        U64 CR3; // Page Table Base Register (untuk virtual memory)

        VOID* StackBase; // Pointer ke base stack (HHDM virtual)
        U64 StackSize;

        U8 Priority; // Prioritas task (0 = tertinggi)
        U64 TimeUsedInPriority;

        Tasking::TaskState::State State; // Current State of the Task
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
    extern U64 ActiveTask;
    extern U64 CurrentTaskIndex;
    extern BOOL SchedulerActive;
    extern U64 g_ForegroundPID;

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
}