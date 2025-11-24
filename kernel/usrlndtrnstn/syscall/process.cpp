#include <rosval.h>
#include <cpu_context.hpp>
#include <rossys.hpp>
#include "syscall/process.hpp"
#define PRINTK_MODULE_NAME "SYSCALL_PROC"
#include <logging.hpp>
#include <task.hpp>
#include "../../mm/kmalloc/kmalloc.hpp"
#include "string.hpp"
#include "../../../misc/file/extension/elf.hpp"
#include "../../filesys/vfs/vfs.hpp"
#include "../../filesys/filesystem.hpp"
#include "../../mm/mm.hpp"
#include "../../mm/usercopy.hpp"

extern "C" VOID Context_Restore(VOID *Context);

// Helper to open file via VFS
static File* VFS_Open(const char* Path) {
    FileSystem* FS = nullptr;
    char RelativePath[256];
    if (!VFSManager::ResolvePath(Path, &FS, RelativePath)) return nullptr;
    return FS->Open(RelativePath);
}

VOID Sys_Exit(CpuContext_T *CPUContext) {
    // Argument 1 is in RBX based on previous entry.cpp code
    UNUSED__ U64 exit_code = CPUContext->rdi;

    //Printk::Write(Printk::Level::LOG_INFO,
    //              "Syscall: Exit called with code %llu\n",
    //              (unsigned long long)exit_code);

    Tasking::Task* curr = Tasking::GetCurrentTaskPtr();
    if (!curr) return;

    switch(curr->pid){
        case 0:
            Printk::Write(Printk::Level::LOG_EMERG,
                          "Syscall: Task IDLE call 60 for exit\n" 
                          "This should not happen!\n"
                          "Please check the condition you have in your PC Hardware:\n"
                          "1. Make sure your RAM and Storage are properly enough\n" 
                          "2. Make sure your CPU is not overheating\n"
                          "3. Make sure you are not running in a VM with insufficient resources\n");
            break;
        case 1:
            Printk::Write(Printk::Level::LOG_EMERG,
                          "Syscall: Task INIT call 60 for exit\n" 
                          "This should not happen!\n"
                          "Please check the condition you have in your PC Hardware:\n"
                          "1. Make sure your RAM and Storage are properly enough\n" 
                          "2. Make sure your CPU is not overheating\n"
                          "3. Make sure you are not running in a VM with insufficient resources\n");
            break;
        default:
            curr->State = Tasking::TaskState::ZOMBIE;
            // Wake up parent if it's blocked in wait
            for (int i = 0; i < MAX_TASK; ++i) {
                Tasking::Task* T = Tasking::TaskArray[i];
                if (!T) continue;
                if (T->pid == curr->ppid && T->State == Tasking::TaskState::BLOCKED) {
                    T->State = Tasking::TaskState::READY;
                    T->Priority = 0;
                    T->TimeSlice = Tasking::GetTimeSliceForPriority(0);
                    T->TimeUsedInPriority = 0;
                }
            }
            Tasking::SchedulerYield();
            UNREACHABLE;
            break;
    }
}

VOID Sys_Fork(CpuContext_T *CPUContext) {
    Tasking::Task* Current = Tasking::GetCurrentTaskPtr();
    
    // 1. Allocate New Task
    Tasking::Task* Child = new Tasking::Task();
    if (!Child) { CPUContext->rax = -1; return; }
    
    // 2. Copy Task Struct
    String::Memcpy(Child, Current, sizeof(Tasking::Task));
    
    // Handle FD Reference Counting
    for (int i = 0; i < MAX_FILE_IN_PROCESS; ++i) {
        if (Child->FDTable[i]) {
            Child->FDTable[i]->RefCount++;
        }
    }
    
    // 3. Allocate New Kernel Stack
    Child->StackBase = Kmalloc::Alloc(Child->StackSize);
    if (!Child->StackBase) { delete Child; CPUContext->rax = -1; return; }
    
    // 4. Clone Address Space
    Child->CR3 = Tasking::CloneUserAddressSpace(Current->CR3);
    if (!Child->CR3) { 
        Kmalloc::Free(Child->StackBase); 
        delete Child; 
        CPUContext->rax = -1; 
        return; 
    }
    
    // 5. Setup Child Context
    // Copy context to the top of the Child's Kernel Stack
    U64 ChildStackTop = (U64)Child->StackBase + Child->StackSize;
    U64 ChildContextAddr = ChildStackTop - sizeof(CpuContext_T);
    
    String::Memcpy((void*)ChildContextAddr, CPUContext, sizeof(CpuContext_T));
    
    // Set Child's RSP to point to this context
    Child->RSP = ChildContextAddr;
    
    // Modify Child's Return Value (RAX) to 0
    CpuContext_T* ChildContext = (CpuContext_T*)ChildContextAddr;
    ChildContext->rax = 0;
    
    // 6. Set PID/PPID
    Child->ppid = Current->pid;
    Child->pid = 0; // Will be set by SchedulerAddTask
    Child->State = Tasking::TaskState::READY;
    
    // 7. Add to Scheduler
    Tasking::SchedulerAddTask(Child);
    
    // 8. Return Child PID to Parent
    CPUContext->rax = Child->pid;
}

VOID Sys_Execve(CpuContext_T *CPUContext) {
    // RDI = Path, RSI = Argv, RDX = Envp
    // const char* Path = (const char*)CPUContext->rbx; // OLD: RBX
    
    Tasking::Task* Current = Tasking::GetCurrentTaskPtr();
    if (!Current) { CPUContext->rax = -1; return; }

    // Debug: print incoming syscall registers to verify argv/envp pointers
    //Printk::Write(Printk::Level::LOG_INFO,
    //              "[SYSCALL_PROC] Enter Sys_Execve: rdi=%p rsi=%p rdx=%p rax=%llx rcx=%llx rbx=%llx rbp=%llx rsp=%p rip=%p\n",
    //              (void*)CPUContext->rdi, (void*)CPUContext->rsi, (void*)CPUContext->rdx,
    //              (unsigned long long)CPUContext->rax, (unsigned long long)CPUContext->rcx,
    //              (unsigned long long)CPUContext->rbx, (unsigned long long)CPUContext->rbp,
    //              (void*)CPUContext->rsp, (void*)CPUContext->rip);

    // Copy Path from User
    char KernelPath[256];
    String::Memset(KernelPath, 0, 256);
    
    U64* UserPML4 = HHDM_PhysToVirt(Current->CR3);
    
    // Use RDI for Path pointer (System V ABI / Linux Syscall Convention)
    // We copy 256 bytes to ensure we get the full path. 
    // TODO: Implement safer StrncpyFromUser to avoid reading past valid pages if string is short.
    if (!PageAlloc::CopyFromUser(UserPML4, KernelPath, (void*)CPUContext->rdi, 256)) {
         // If strict copy fails, we might want to try copying byte-by-byte or handle it better.
         // For now, assume failure means bad pointer.
         CPUContext->rax = -1;
         return;
    }
    // Ensure null termination
    KernelPath[255] = '\0';
    
    // 1. Open File
    File* F = VFS_Open(KernelPath);
    if (!F) { CPUContext->rax = -1; return; }
    
    // 2. Read File (Load ELF)
    U64 Size = F->FileSize;
    void* Buffer = Kmalloc::Alloc(Size);
    if (!Buffer) { F->FSOwner->Close(F); CPUContext->rax = -1; return; }
    
    if (F->FSOwner->Read(F, (U8*)Buffer, Size) != Size) {
        Kmalloc::Free(Buffer);
        F->FSOwner->Close(F);
        CPUContext->rax = -1;
        return;
    }
    F->FSOwner->Close(F);
    
    // 3. Create New Address Space
    U64 NewCR3 = Tasking::CreateUserAddressSpace();
    if (!NewCR3) { Kmalloc::Free(Buffer); CPUContext->rax = -1; return; }
    
    U64* NewPML4 = HHDM_PhysToVirt(NewCR3);
    
    // 4. Load ELF into New Address Space
    U64 ImageBase, ImageEnd;
    U64 Entry = ELF::LoadELF64(Buffer, NewPML4, &ImageBase, &ImageEnd);
    Kmalloc::Free(Buffer); // Done with buffer
    
    if ((I64)Entry < 0) {
        // Failed. 
        // TODO: Free NewCR3
        CPUContext->rax = -1;
        return;
    }
    
    // 5. Setup Stack (with argv/argc)
    // Map pages for user stack and track their physical frames so we can
    // write argc/argv and strings into the newly mapped user stack.
    // Use the same high canonical user stack top as TaskUserConstructor.
    constexpr U64 USER_STACK_TOP = 0x00007FFFFFFFE000ULL;
    constexpr U64 USER_STACK_PAGES = 8;

    U64 InitialRSP = USER_STACK_TOP;
    UPTR stack_phys[USER_STACK_PAGES];
    for (SIZE_T i = 0; i < USER_STACK_PAGES; ++i) stack_phys[i] = 0;

    U64 stack_base = USER_STACK_TOP - (USER_STACK_PAGES * 0x1000);

    for (SIZE_T i = 0; i < USER_STACK_PAGES; ++i) {
        UPTR phys = PageAlloc::PhysicalAllocPages(1);
        if (phys) {
            U64 virt = stack_base + (i * 0x1000);
            PageAlloc::MapPages(NewPML4, phys, virt, 1, 0x7); // User RW
            stack_phys[i] = phys;
        } else {
            // mapping failed - cleanup would be ideal but bail for now
            CPUContext->rax = -1;
            return;
        }
    }

    // We'll build the stack top-down. `cur` is the next free byte (grows down).
    U64 cur = USER_STACK_TOP;

    // Helper lambdas
    auto push_bytes_to_newstack = [&](const void* src, SIZE_T len) -> U64 {
        // Allocate space
        cur -= (len);
        // Align to 8
        cur &= ~((U64)0x7);

        // Compute where in phys pages to write
        U64 offset = cur - stack_base;
        while (len > 0) {
            U64 page_index = offset / 0x1000;
            U64 page_off = offset % 0x1000;
            U64 can_copy = 0x1000 - page_off;
            if (can_copy > len) can_copy = len;
            void* dst = (void*)((UPTR)HHDM_PhysToVirt(stack_phys[page_index]) + page_off);
            String::Memcpy(dst, src, (unsigned long long)can_copy);
            src = (const void*)((const char*)src + can_copy);
            len -= can_copy;
            offset += can_copy;
        }
        return cur;
    };

    // Copy argv and envp strings from the old user address space (UserPML4)
    // into the new user stack. Build argv and envp pointer arrays on the stack.
    constexpr SIZE_T MAX_ARGC = 64;
    constexpr SIZE_T MAX_ENVP = 128;
    U64 argv_ptrs[MAX_ARGC];
    U64 envp_ptrs[MAX_ENVP];
    SIZE_T argc = 0;
    SIZE_T envc = 0;

    // CPUContext->rsi is argv (user pointer to char*[]), CPUContext->rdx is envp
    U64 user_argv = CPUContext->rsi;
    U64 user_envp = CPUContext->rdx;

    // Probe logs temporarily disabled to reduce noisy kernel output.
    // If you need them again, re-enable by removing the surrounding
    // comment markers.
    /*
    if (user_argv != 0) {
        U64 probe = 0;
        BOOL ok = PageAlloc::CopyFromUser(UserPML4, &probe, (void*)user_argv, sizeof(probe));
        if (!ok) {
            Printk::Write(Printk::Level::LOG_ERR, "[SYSCALL_PROC] probe: CopyFromUser(user_argv=%p) FAILED\n", (void*)user_argv);
        } else {
            Printk::Write(Printk::Level::LOG_INFO, "[SYSCALL_PROC] probe: user_argv@%p -> %p\n", (void*)user_argv, (void*)probe);
        }
        // Also try a couple of nearby addresses
        for (int pi = 1; pi <= 3; ++pi) {
            U64 paddr = user_argv + pi * sizeof(U64);
            U64 pval = 0;
            BOOL ok2 = PageAlloc::CopyFromUser(UserPML4, &pval, (void*)paddr, sizeof(pval));
            if (!ok2) Printk::Write(Printk::Level::LOG_ERR, "[SYSCALL_PROC] probe: CopyFromUser(%p) FAILED\n", (void*)paddr);
            else Printk::Write(Printk::Level::LOG_INFO, "[SYSCALL_PROC] probe: %p -> %p\n", (void*)paddr, (void*)pval);
        }
    }
    */

    // Helper to copy a user string (up to bufsize) into a temp buffer then
    // push into newstack and return destination virtual address
    auto copy_user_string_to_newstack = [&](U64 user_ptr) -> U64 {
        if (user_ptr == 0) return 0;
        char temp[1024];
        SIZE_T idx = 0;

        // Copy one byte at a time until NUL or buffer full. This avoids
        // attempting to read across the user stack top into an unmapped page
        // when the original bulk copy would span the 4GiB boundary.
        for (; idx < (sizeof(temp) - 1); ++idx) {
            char ch = 0;
            if (!PageAlloc::CopyFromUser(UserPML4, &ch, (void*)(user_ptr + idx), 1)) {
                Printk::Write(Printk::Level::LOG_ERR, "[SYSCALL_PROC] copy_user_string_to_newstack: byte CopyFromUser(%p) FAILED at offset %llu\n",
                              (void*)(user_ptr + idx), (unsigned long long)idx);
                // Provide page-level diagnostics for the failing address
                PageAlloc::DumpVaddrMapping(UserPML4, (UPTR)(user_ptr + idx));
                return 0;
            }
            temp[idx] = ch;
            if (ch == '\0') break;
        }
        temp[idx] = '\0';
        SIZE_T slen = (SIZE_T)String::Strnlen(temp, sizeof(temp));
        // debug first few characters
        UNUSED__ char dbg[32];
        SIZE_T dbglen = (slen < 16) ? slen : 16;
        for (SIZE_T di = 0; di < dbglen; ++di) dbg[di] = temp[di];
        dbg[dbglen] = '\0';
        //Printk::Write(Printk::Level::LOG_INFO, "[SYSCALL_PROC] copy_user_string_to_newstack: user_ptr=%p len=%llu preview='%s'\n",
        //              (void*)user_ptr, (unsigned long long)slen, dbg);
        return push_bytes_to_newstack(temp, slen + 1);
    };

    if (user_argv != 0) {
        for (SIZE_T ai = 0; ai < MAX_ARGC; ++ai) {
            U64 user_arg_ptr = 0;
            if (!PageAlloc::CopyFromUser(UserPML4, &user_arg_ptr, (void*)(user_argv + ai * sizeof(U64)), sizeof(U64))) break;
            if (user_arg_ptr == 0) break; // NULL terminator
            U64 dest_addr = copy_user_string_to_newstack(user_arg_ptr);
            if (dest_addr == 0) {
                Printk::Write(Printk::Level::LOG_ERR, "[SYSCALL_PROC] argv: failed to copy string at %p\n", (void*)user_arg_ptr);
                break;
            }
            argv_ptrs[argc++] = dest_addr;
        }
    }

    if (user_envp != 0) {
        for (SIZE_T ei = 0; ei < MAX_ENVP; ++ei) {
            U64 user_env_ptr = 0;
            if (!PageAlloc::CopyFromUser(UserPML4, &user_env_ptr, (void*)(user_envp + ei * sizeof(U64)), sizeof(U64))) break;
            if (user_env_ptr == 0) break;
            U64 dest_addr = copy_user_string_to_newstack(user_env_ptr);
            if (dest_addr == 0) {
                Printk::Write(Printk::Level::LOG_ERR, "[SYSCALL_PROC] envp: failed to copy string at %p\n", (void*)user_env_ptr);
                break;
            }
            envp_ptrs[envc++] = dest_addr;
        }
    }

    // Now build pointer arrays on stack in this order (low->high):
    // argc, argv[0..], NULL, envp[0..], NULL, (then strings already placed above)
    // We must push in reverse order: push envp NULL, envp pointers (reverse),
    // push argv NULL, argv pointers (reverse), then argc.

    U64 zero = 0;

    // push envp NULL
    cur -= sizeof(U64);
    cur &= ~((U64)0x7);
    {
        U64 offset = cur - stack_base;
        U64 page_index = offset / 0x1000;
        U64 page_off = offset % 0x1000;
        void* dst = (void*)((UPTR)HHDM_PhysToVirt(stack_phys[page_index]) + page_off);
        String::Memcpy(dst, &zero, sizeof(U64));
    }

    // push envp pointers in reverse
    U64 envp_user_ptr = 0;
    for (SIZE_T k = 0; k < envc; ++k) {
        U64 v = envp_ptrs[envc - 1 - k];
        cur -= sizeof(U64);
        cur &= ~((U64)0x7);
        U64 offset = cur - stack_base;
        U64 page_index = offset / 0x1000;
        U64 page_off = offset % 0x1000;
        void* dst = (void*)((UPTR)HHDM_PhysToVirt(stack_phys[page_index]) + page_off);
        String::Memcpy(dst, &v, sizeof(U64));
    }
    // remember pointer to start of envp array (if any)
    if (envc > 0) envp_user_ptr = cur;

    // push argv NULL
    cur -= sizeof(U64);
    cur &= ~((U64)0x7);
    {
        U64 offset = cur - stack_base;
        U64 page_index = offset / 0x1000;
        U64 page_off = offset % 0x1000;
        void* dst = (void*)((UPTR)HHDM_PhysToVirt(stack_phys[page_index]) + page_off);
        String::Memcpy(dst, &zero, sizeof(U64));
    }

    // push argv pointers in reverse
    for (SIZE_T k = 0; k < argc; ++k) {
        U64 v = argv_ptrs[argc - 1 - k];
        cur -= sizeof(U64);
        cur &= ~((U64)0x7);
        U64 offset = cur - stack_base;
        U64 page_index = offset / 0x1000;
        U64 page_off = offset % 0x1000;
        void* dst = (void*)((UPTR)HHDM_PhysToVirt(stack_phys[page_index]) + page_off);
        String::Memcpy(dst, &v, sizeof(U64));
    }

    U64 argv_user_ptr = 0;
    if (argc > 0) argv_user_ptr = cur;

    // Finally, push argc
    U64 argc_val = (U64)argc;
    cur -= sizeof(U64);
    cur &= ~((U64)0x7);
    {
        U64 offset = cur - stack_base;
        U64 page_index = offset / 0x1000;
        U64 page_off = offset % 0x1000;
        void* dst = (void*)((UPTR)HHDM_PhysToVirt(stack_phys[page_index]) + page_off);
        String::Memcpy(dst, &argc_val, sizeof(U64));
    }

    InitialRSP = cur;

    // Debug: dump argc/argv/envp that we've prepared (helps debug user programs)
    //Printk::Write(Printk::Level::LOG_INFO, "[SYSCALL_PROC] Debug: prepared argc=%llu envc=%llu InitialRSP=%p\n",
    //              (unsigned long long)argc, (unsigned long long)envc, (void*)InitialRSP);
    for (SIZE_T i = 0; i < argc; ++i) {
        U64 saddr = argv_ptrs[i];
        // locate physical page and offset
        if (saddr >= stack_base && saddr < USER_STACK_TOP) {
            U64 soff = saddr - stack_base;
            U64 page_index = soff / 0x1000;
            U64 page_off = soff % 0x1000;
            UNUSED__ char tmp[256];
            char *src = (char*) ((UPTR)HHDM_PhysToVirt(stack_phys[page_index]) + page_off);
            // copy up to 255 chars
            SIZE_T j = 0;
            while (j < 255 && src[j] != '\0') { tmp[j] = src[j]; ++j; }
            tmp[j] = '\0';
            //Printk::Write(Printk::Level::LOG_INFO, "[SYSCALL_PROC] argv[%u]=%p -> %s\n", (unsigned)i, (void*)saddr, tmp);
        } else {
            Printk::Write(Printk::Level::LOG_ERR, "[SYSCALL_PROC] argv[%u]=%p (out-of-stack)\n", (unsigned)i, (void*)argv_ptrs[i]);
        }
    }
    for (SIZE_T i = 0; i < envc; ++i) {
        U64 saddr = envp_ptrs[i];
        if (saddr >= stack_base && saddr < USER_STACK_TOP) {
            U64 soff = saddr - stack_base;
            U64 page_index = soff / 0x1000;
            U64 page_off = soff % 0x1000;
            char tmp[256];
            char *src = (char*) ((UPTR)HHDM_PhysToVirt(stack_phys[page_index]) + page_off);
            SIZE_T j = 0;
            while (j < 255 && src[j] != '\0') { tmp[j] = src[j]; ++j; }
            tmp[j] = '\0';
            Printk::Write(Printk::Level::LOG_INFO, "[SYSCALL_PROC] envp[%u]=%p -> %s\n", (unsigned)i, (void*)saddr, tmp);
        } else {
            Printk::Write(Printk::Level::LOG_ERR, "[SYSCALL_PROC] envp[%u]=%p (out-of-stack)\n", (unsigned)i, (void*)envp_ptrs[i]);
        }
    }
    
    // 6. Switch Address Space
    // Tasking::Task* Current = Tasking::GetCurrentTaskPtr(); // Already defined at top
    U64 OldCR3 = Current->CR3;
    
    // Move context to Kernel Stack before switching CR3
    U64 KernelStackTop = (U64)Current->StackBase + Current->StackSize;
    U64 NewContextAddr = KernelStackTop - sizeof(CpuContext_T);
    String::Memcpy((void*)NewContextAddr, CPUContext, sizeof(CpuContext_T));
    CpuContext_T* NewContext = (CpuContext_T*)NewContextAddr;

    // Update Context
    NewContext->rip = Entry;
    NewContext->rsp = InitialRSP;
    NewContext->cs = 0x4B;
    NewContext->ss = 0x43;
    NewContext->rflags = 0x202;
    NewContext->rax = 0;
    // Set up argc/argv registers per SysV for the new program
    NewContext->rdi = argc_val;
    NewContext->rsi = argv_user_ptr;
        NewContext->rdx = envp_user_ptr; // Set envp to user pointer

    Current->CR3 = NewCR3;
    DoCR3::Load((U64*)NewCR3);

    Tasking::FreeUserAddressSpace(OldCR3);

    // Restore from Kernel Stack (never returns to wrapper)
    Context_Restore(NewContext);
}

VOID Sys_Wait(CpuContext_T *CPUContext) {
    Tasking::Task* Current = Tasking::GetCurrentTaskPtr();
    if (!Current) { CPUContext->rax = -1; return; }

    while (true) {
        // 1. Scan ZOMBIE
        bool HasChildren = false;
        for (int i = 0; i < MAX_TASK; ++i) {
            Tasking::Task* T = Tasking::TaskArray[i];
            if (T && T->ppid == Current->pid) {
                HasChildren = true;
                if (T->State == Tasking::TaskState::ZOMBIE) {
                    // Normal Exit (SysExit)
                    U64 ChildPID = T->pid;
                    Tasking::DestroyTask(T);
                    CPUContext->rax = ChildPID;
                    return;
                }
            }
        }

        // 2. Cek apakah anak Tiba-Tiba Hilang? (Kasus SIGINT)
        // Kalau 'HasChildren' jadi false padahal tadi true, berarti anak mati paksa.
        // TAPI, loop di atas sudah ngecek kondisi real-time.
        
        if (!HasChildren) { 
            // Gak punya anak sama sekali.
            // Ini bisa terjadi kalau anak dimatiin paksa oleh SchedulerTick (SIGINT)
            // dan langsung dihapus dari TaskArray.
            // Parent harusnya return -1 (ECHILD) biar shell tau "Oh, anak gw dah abis".
            CPUContext->rax = -1; 
            return; 
        }

        // 3. Tidur
        LOCKRFLAGS irq = Arch::SaveAndDisableInterrupts();
        Current->State = Tasking::TaskState::BLOCKED;
        Arch::RestoreInterrupts(irq);

        Tasking::SchedulerYield();
        
        // [FIX] Pas bangun, kalau ternyata anak hilang karena SIGINT, loop akan muter ke atas,
        // HasChildren jadi false, dan return -1. Shell akan nerima -1 dan lanjut jalan.
    }
}

VOID Sys_GetPID(CpuContext_T *CPUContext) {
    Tasking::Task* Current = Tasking::GetCurrentTaskPtr();
    if (!Current) { CPUContext->rax = -1; return; }
    CPUContext->rax = Current->pid;
}

VOID Sys_SetPGID(CpuContext_T *CPUContext){
    U64 pid = CPUContext->rdi;
    U64 pgid = CPUContext->rsi; 

    Tasking::Task *TargetTask = nullptr;
    Tasking::Task *Current = Tasking::GetCurrentTaskPtr();

    if(pid == 0){
        TargetTask = Current;
    } else {
        TargetTask = Tasking::GetTaskPID(pid);
    }

    if(!TargetTask){
        CPUContext->rax = -3;
        return;
    }

    if(pgid == 0){
        pgid = TargetTask->pid;
    }

    TargetTask->PGID = pgid;

    Printk::Write(Printk::Level::LOG_NOTICE, "PID %d join proccess group %d\n", TargetTask->pid, pgid);

    CPUContext->rax = 0;
}

VOID Sys_GetPGID(CpuContext_T *CPUContext){
    U64 pid = CPUContext->rdi;

    Tasking::Task *TargetTask = nullptr;

    if(pid == 0){
        TargetTask = Tasking::GetCurrentTaskPtr();
    } else {
        TargetTask = Tasking::GetTaskPID(pid);
    }

    if(!TargetTask){
        CPUContext->rax = -3;
        return;
    }

    CPUContext->rax = TargetTask->PGID;
}