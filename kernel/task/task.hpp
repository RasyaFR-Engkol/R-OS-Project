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
struct File;

// VMA protection flags (used in `VMArea::Prot`)
#define VMA_READ   (1ULL << 0)
#define VMA_WRITE  (1ULL << 1)
#define VMA_EXEC   (1ULL << 2)
#define VMA_SHARED (1ULL << 3)

// Mapping flags (used in `VMArea::Flags` / mmap-like interfaces)
#define MAP_SHARED    (1ULL << 0)
#define MAP_PRIVATE   (1ULL << 1)
#define MAP_FIXED     (1ULL << 2)
#define MAP_ANONYMOUS (1ULL << 3)
#define MAP_STACK     (1ULL << 4) // Hint: mapping is intended for thread/user stack

#define MAX_FILE_IN_PROCESS 64
#define MLFQ_LEVELS 4
#define PRIORITY_BOOST_INTERVAL 1000

#define TIMER_WHEEL_SIZE 512 // Harus power of 2

// Ini harusnya adjustable terhadap kebutuhan sistem operasi kita
// tapi untuk sekarang kita tetapkan 256 Task saja.
#define MAX_TASK 256

// FLAGS MLFQ
#define TASK_SLEEPING (1 << 0)

// flags POLLIN
// Konstanta bitmask poll (Standar)
#define POLLIN      0x0001
#define POLLPRI     0x0002
#define POLLOUT     0x0004
#define POLLERR     0x0008
#define POLLHUP     0x0010
#define POLLNVAL    0x0020

extern VOLATILE U32 PriorityBitmap;

namespace Tasking {
    constexpr U64 PID_IDLE = 0;
    constexpr U64 PID_INIT = 1;
    constexpr U64 PID_USER_START = 10; // User process mulai dari sini
}

extern VOLATILE U64 GlobalBoostEpoch;

// tasking FLAGS
#define PERM_ESSENTIAL_SYSTEM   (1 << 1) // Gak bisa di-kill sembarangan
#define PERM_ADMIN_SUDO         (1 << 2) // Punya akses root/kernel space helper
#define PERM_SYS_CRITICAL       (1 << 3) // SCHEDULER: Selalu Priority 0, Anti-Turun Kasta

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

    struct VMArea{
        U64 Start;
        U64 End;
        U64 Prot;
        U64 Flags;
        File *BackingFile;
        U64 FileOffset;
        VMArea *Next;
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
        U8 FPU_Storage[512 + 16]; 
        
        // Pointer inilah yang akan kita pass ke fxsave/fxrstor
        U8* FPU_Region;

        U8 Priority; // Prioritas task (0 = tertinggi)
        U64 TimeUsedInPriority;

        VOLATILE Tasking::TaskState::State State; // Current State of the Task
        UFLAGS BlockReason; 

        // untuk tidur berapa ya tick nya?
        VOLATILE U64 SleepTick;
        
        Tasking::Task *NextWaitTask;

        U64 TimeSlice; // Time slice for scheduling
        U64 SleepUntil; // Waktu hingga task ini harus dibangunkan (jika tidur)

        Tasking::Task *NextTask; // Pointer to the next task in the scheduler's list

        File *FDTable[MAX_FILE_IN_PROCESS]; // File Descriptor Table

        VMArea *VMHead;
        U64 MMapNextAddr; // Untuk syscall mmap, nyimpen alamat mmap berikutnya

        VOLATILE BOOL YieldRequested; // Tambah ini

        CHAR8 CWD[256];
        BOOL IsSudoOrAdmin = FALSE;
        BOOL IsCriticalProc = FALSE;
        BOOL IsEssentialSystem = FALSE;

        Task *NextRunQueue = nullptr;
        Task *PrevRunQueue = nullptr;
        Task *NextSleepQueue = nullptr;
        Task* NextReady = nullptr;
        U64 LastBoostEpoch = 0;

        U64 CountMinorFault = 0;
        U64 CountMajorFault = 0;
    };

    struct RunQueue{
        Task *Head = nullptr;
        Task *Tail = nullptr;
        U64 Count = 0;
    };

    struct WaitQueue {
        Task *Head = nullptr;
        Task *Tail = nullptr;
    };

    // Variabl global untuk task management
    extern Task *TaskArray[MAX_TASK];
    extern Task *GraveyardArray[MAX_TASK];
    extern U64 ActiveTask;
    extern U64 CurrentTaskIndex;
    extern VOLATILE BOOL SchedulerActive;
    extern U64 g_ForegroundPID;
    extern VOLATILE BOOL ForceReschedule;

    VOID SchedulerStart();
    VOID CreateKThread(VOID (*Entry)(VOID), const char* taskname);
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
    VOID SleepOn(WaitQueue &queue);
    VOID WakeUp(WaitQueue &queue);
    VOID WakeUpAll(WaitQueue &queue);
    short CheckFileDesc(int fd, short events);
    VOID Enqueue(Task *t);
    VOID AddToSleepList(Task *t);
    VOID SettingAppPerm(Task *t, U32 Perm);


    Task *ConstructTask(VOID (*Entry)(VOID), const char *taskname);
    VOID ReapDTask();
    VOID Sleep(U64 ms);
    VOID Debug_DumpProcessState();
    VOID Debug_DumpFDProccessBelowPID10();
    VOID Debug_MinorAndMajorFaultsBelowPID10();
}