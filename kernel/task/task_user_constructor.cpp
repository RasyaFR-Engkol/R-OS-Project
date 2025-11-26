#include <rossys.hpp>
#include <rosval.h>
#include <logging.hpp>
#include "string.hpp"
#include "task.hpp"
#include "cpu_context.hpp"
#include "../mm/mm.hpp"
#include "../mm/kmalloc/kmalloc.hpp"
#include "../../misc/file/extension/elf.hpp"

namespace Tasking {
namespace {
constexpr SIZE_T KERNEL_STACK_SIZE = 0x4000;      // 16 KiB per kernel task stack
constexpr SIZE_T USER_STACK_PAGES = 8;            // 32 KiB user stack by default
// Place user stacks in the high canonical user area so there's ample room
// below for growth and we avoid the 4GiB boundary. Use a page-aligned top
// similar to common Linux layout.
constexpr U64    USER_STACK_TOP   = 0x00007FFFFFFFE000ULL; // high canonical user stack top
// Updated to match GDT indices 9 (code) and 8 (data): selector = (index<<3) | RPL(3)
UNUSED__ constexpr U16    USER_CODE_SELECTOR = 0x4B; // index 9, RPL=3
UNUSED__ constexpr U16    USER_DATA_SELECTOR = 0x43; // index 8, RPL=3

struct UserStackState {
    U64 bottom;
    SIZE_T mappedPages;
    UPTR physPages[USER_STACK_PAGES];
};

static inline U64 AlignUp(U64 value, U64 alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static bool MapUserStack(U64 *userPml4, UserStackState &state) {
    state.bottom = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;
    state.mappedPages = 0;

    for (SIZE_T i = 0; i < USER_STACK_PAGES; ++i) {
        UPTR phys = PageAlloc::PhysicalAllocPages(1);
        if (!phys) {
            return false;
        }

        state.physPages[state.mappedPages++] = phys;
        auto *clearPtr = reinterpret_cast<U8*>(HHDM_PhysToVirt(phys));
        String::Memset(clearPtr, 0, PAGE_SIZE);

        U64 virt = state.bottom + i * PAGE_SIZE;
        if (!PageAlloc::MapPages(userPml4, phys, virt, 1,
                                 PAGE_PRESENT | PAGE_RW | PAGE_USER)) {
            PageAlloc::PhysicalFreePages(phys, 1);
            state.mappedPages--;
            return false;
        }
    }

    return true;
}

static void RollbackUserStack(U64 *userPml4, const UserStackState &state) {
    for (SIZE_T i = 0; i < state.mappedPages; ++i) {
        U64 virt = state.bottom + i * PAGE_SIZE;
        PageAlloc::UnMapPages(userPml4, virt);
        PageAlloc::PhysicalFreePages(state.physPages[i], 1);
    }
}
}

static void FreeKernelStack(Task *task) {
    if (task && task->StackBase) {
        Kmalloc::Free(task->StackBase);
        task->StackBase = nullptr;
    }
}

Task* TaskUserConstructor(const CHAR8 *Name, VOID *ELFImage) {
    if (!ELFImage) {
        Printk::Write(Printk::Level::LOG_ERR, "TaskUserConstructor: ELF image is null\n");
        return nullptr;
    }

    Task *newTask = new Task();
    if (!newTask) {
        Printk::Write(Printk::Level::LOG_ERR, "TaskUserConstructor: failed to allocate Task struct\n");
        return nullptr;
    }
    String::Memset(newTask, 0, sizeof(Task));

    newTask->StackSize = KERNEL_STACK_SIZE;
    newTask->StackBase = Kmalloc::Alloc(newTask->StackSize);
    if (!newTask->StackBase) {
        Printk::Write(Printk::Level::LOG_ERR, "TaskUserConstructor: failed to allocate kernel stack\n");
        delete newTask;
        return nullptr;
    }

    U64 userCr3Phys = CreateUserAddressSpace();
    if (!userCr3Phys) {
        Printk::Write(Printk::Level::LOG_ERR, "TaskUserConstructor: failed to allocate user address space\n");
        FreeKernelStack(newTask);
        delete newTask;
        return nullptr;
    }
    U64 *userPml4 = HHDM_PhysToVirt(userCr3Phys);

    UserStackState stackState{};
    if (!MapUserStack(userPml4, stackState)) {
        Printk::Write(Printk::Level::LOG_ERR, "TaskUserConstructor: failed to map user stack\n");
        RollbackUserStack(userPml4, stackState);
        PageAlloc::PhysicalFreePages(userCr3Phys, 1);
        FreeKernelStack(newTask);
        delete newTask;
        return nullptr;
    }

    U64 imageBase = 0;
    U64 imageEnd = 0;
    U64 entry = ELF::LoadELF64(ELFImage, userPml4, &imageBase, &imageEnd);
    if ((VAL64)entry < 0) {
        Printk::Write(Printk::Level::LOG_ERR, "TaskUserConstructor: ELF load failed (err=%lld)\n", (VAL64)entry);
        RollbackUserStack(userPml4, stackState);
        PageAlloc::PhysicalFreePages(userCr3Phys, 1);
        FreeKernelStack(newTask);
        delete newTask;
        return nullptr;
    }

    U64 userStackTop = stackState.bottom + USER_STACK_PAGES * PAGE_SIZE;
    U64 stackFrameAddr = (U64)newTask->StackBase + newTask->StackSize - sizeof(CpuContext_T);
    CpuContext_T *frame = reinterpret_cast<CpuContext_T*>(stackFrameAddr);
    String::Memset(frame, 0, sizeof(CpuContext_T));

    frame->rip = entry;
    frame->cs = 0x4B;
    frame->rflags = 0x202;
    frame->rsp = userStackTop;
    frame->ss = 0x43;

    // Debug print to verify selectors
    Printk::Write(Printk::Level::LOG_INFO, "TaskUserConstructor: Created task '%s' with CS=0x%x SS=0x%x RSP=0x%llx\n",
                  Name ? Name : "UserTask", (unsigned)frame->cs, (unsigned)frame->ss, (unsigned long long)frame->rsp);

    newTask->RSP = stackFrameAddr;
    newTask->CR3 = userCr3Phys;
    newTask->pid = 0;
    newTask->ppid = 0;
    newTask->NextTask = nullptr;
    newTask->Priority = 0;
    newTask->TimeSlice = GetTimeSliceForPriority(0);
    newTask->TimeUsedInPriority = 0;
    newTask->State = TaskState::READY;
    newTask->SleepUntil = 0;
    newTask->YieldRequested = FALSE;
    newTask->Signals = 0;
    newTask->PGID = newTask->pid;
    constexpr U64 USER_HEAP_START = 0x0000000200000000ULL;
    newTask->MMapNextAddr = AlignUp(imageEnd, PAGE_SIZE);
    if (newTask->MMapNextAddr < USER_HEAP_START) {
        newTask->MMapNextAddr = USER_HEAP_START;
    }

    if (Name && Name[0] != '\0') {
        String::Strncpy(newTask->Name, Name, sizeof(newTask->Name) - 1);
        newTask->Name[sizeof(newTask->Name) - 1] = '\0';
    } else {
        String::Strcpy(newTask->Name, "UserTask");
    }

    // setup file descriptor table ke stdin, stdout, stderr
    newTask->FDTable[0] = VFSManager::Open("/dev/tty"); // stdin
    newTask->FDTable[1] = VFSManager::Open("/dev/tty"); // stdout
    newTask->FDTable[2] = VFSManager::Open("/dev/tty"); // stderr
    newTask->CWD[0] = '/'; newTask->CWD[1] = '\0';

    newTask->PGIDTaskPtr = nullptr;

    return newTask;
}

VOID CreateUserTask(const CHAR8 *Name, VOID *ELFImage) {
    Task *task = TaskUserConstructor(Name, ELFImage);
    if (!task) {
        Printk::Write(Printk::Level::LOG_ERR, "CreateUserTask: failed to construct task\n");
        return;
    }

    SchedulerAddTask(task);
}
}
