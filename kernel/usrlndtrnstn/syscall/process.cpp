#include <rosval.h>
#include <cpu_context.hpp>
#include <rossys.hpp>
#include "rostime.hpp"
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
#include <../firmware/acpi/driver/timer/timer.hpp>

extern "C" VOID Context_Restore(VOID *Context);

// Helper to open file via VFS
static File* VFS_Open(const char* Path, U32 flags) {
    FileSystem* FS = nullptr;
    char RelativePath[256];
    if (!VFSManager::ResolvePath(Path, &FS, RelativePath)) {
        Printk::Write(Printk::Level::LOG_DEBUG, "VFS_Open - Failed to resolve path %s\n", Path);
        return nullptr;
    }
    return VFSManager::Open(RelativePath, flags);
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

    Child->VMHead = nullptr;
    Tasking::VMArea *Node = Current->VMHead;
    while(Node){
        Tasking::VMArea* NewNode = new Tasking::VMArea();
        *NewNode = *Node; // Copy isi datanya (Start, End, Flags)
        NewNode->Next = nullptr; // Putus rantai copy-an
        
        // Masukkan ke list Child
        if (!Child->VMHead) {
            Child->VMHead = NewNode;
        } else {
            Tasking::VMArea* Temp = Child->VMHead;
            while(Temp->Next) Temp = Temp->Next;
            Temp->Next = NewNode;
        }
        
        Node = Node->Next;
    }
    
    // Set Child's RSP to point to this context
    Child->RSP = ChildContextAddr;
    
    // Modify Child's Return Value (RAX) to 0
    CpuContext_T* ChildContext = (CpuContext_T*)ChildContextAddr;
    ChildContext->rax = 0;
    
    // 6. Set PID/PPID/PGID
    Child->NextRunQueue = nullptr;
    Child->PrevRunQueue = nullptr;
    Child->NextSleepQueue = nullptr;
    Child->NextWaitTask = nullptr;
    Child->ppid = Current->pid;
    Child->pid = (U64)-1; // Will be set by SchedulerAddTask
    Child->State = Tasking::TaskState::READY;
    Child->Priority = 0;
    Child->TimeSlice = Tasking::GetTimeSliceForPriority(0);
    Child->TimeUsedInPriority = 0;
    Child->LastBoostEpoch = GlobalBoostEpoch; // Sync epoch biar gak langsung reset lagi
    Child->PGID = Current->PGID;
    Child->PGIDTaskPtr = nullptr; // Safety awal
    Child->IsCriticalProc = FALSE; // Default non-critical
    Child->IsEssentialSystem = FALSE; // Default non-essential
    Child->IsSudoOrAdmin = FALSE; // Default no sudo/admin

    // Kalau dia masuk ke grup (PGID != 0), kita harus update rantai
    if (Child->PGID != 0) {
        // Kita cari Leader gengnya
        Tasking::Task* Leader = Tasking::GetTaskPID(Child->PGID);
        
        if (Leader) {
            // TEKNIK INSERT AT HEAD (Paling Cepat O(1))
            // 1. Child nunjuk ke temen yg ditunjuk Leader sebelumnya
            Child->PGIDTaskPtr = Leader->PGIDTaskPtr;
            
            // 2. Leader nunjuk ke Child (Anak baru jadi tangan kanan Leader)
            Leader->PGIDTaskPtr = Child;
        } else {
            // Edge case aneh: Leader mati pas fork?
            // Ya udah Child jadi solo player dulu
            Child->PGID = 0;
            Child->PGIDTaskPtr = nullptr;
        }
    }
    
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
        Printk::Write(Printk::Level::LOG_DERR, "FAILED WHILE COPYING FROM USER.\n");
         CPUContext->rax = -1;
         return;
    }
    // Ensure null termination
    KernelPath[255] = '\0';
    // Set process task name to basename of the executable (e.g. "compositor.elf")
    {
        char base[64];
        String::Memset(base, 0, sizeof(base));
        String::Basename(KernelPath, base, sizeof(base));
        // Truncate and copy into Task name (leave room for NUL)
        if (Current) {
            String::Strncpy(Current->Name, base, sizeof(Current->Name) - 1);
            Current->Name[sizeof(Current->Name) - 1] = '\0';
        }
    }
    
    // 1. Open File
    File* F = VFS_Open(KernelPath, O_RDONLY);
    if (!F) { 
        Printk::Write(Printk::Level::LOG_DERR, "FAILED WHILE OPENING FROM VFS: No such file or directory.\n");
        CPUContext->rax = -1; return; 
    }
    
    // 2. Read File (Load ELF)
    U64 Size = F->Node->FileSize;
    void* Buffer = Kmalloc::Alloc(Size);
    if (!Buffer) { 
        Printk::Write(Printk::Level::LOG_DERR, "FAILED WHILE ALLOCATING BUFFER: Run out of memory.\n");
        F->Node->FSOwner->Close(F); 
        CPUContext->rax = -1; return; 
    }
    
    if (F->Node->FSOwner->Read(F, (U8*)Buffer, Size) != Size) {
        Printk::Write(Printk::Level::LOG_DERR, "FAILED WHILE READING FROM VFS: Unknown.\n");
        Kmalloc::Free(Buffer);
        F->Node->FSOwner->Close(F);
        CPUContext->rax = -1;
        return;
    }
    F->Node->FSOwner->Close(F);
    
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
        
        Tasking::FreeUserAddressSpace(NewCR3);
        PageAlloc::PhysicalFreePages(NewCR3, 1);
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
    
    // =========================================================================
    // 6. UPDATE PROCESS METADATA (VMA & Address Space)
    // =========================================================================
    using namespace Tasking;

    // A. Bersihkan VMA Lama (PENTING! Jangan sampai sampah fork tertinggal)
    // Kita harus menghapus semua node VMA lama agar bersih.
    VMArea* OldNode = Current->VMHead;
    while (OldNode) {
        VMArea* Next = OldNode->Next;
        delete OldNode; // Asumsi kamu punya operator delete kernel
        OldNode = Next;
    }
    Current->VMHead = nullptr; // Reset Head

    // B. Catat VMA Baru: IMAGE (Code/Data dari ELF)
    // ImageBase & ImageEnd didapat dari output ELF::LoadELF64 di atas
    VMArea* ElfVma = new VMArea();
    if (ElfVma) {
        // Align Up End address ke Page Size
        U64 AlignedEnd = (ImageEnd + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        
        ElfVma->Start = ImageBase;
        ElfVma->End   = AlignedEnd;
        // Simplifikasi permissions: RWX (Read Write Exec). 
        // Idealnya baca dari ELF Header per segmen, tapi RWX cukup untuk run.
        ElfVma->Prot  = 0x7; 
        ElfVma->Flags = 0x02; // MAP_PRIVATE
        ElfVma->Next  = nullptr;
        
        Current->VMHead = ElfVma; // Masukkan sebagai Head
    }

    // C. Catat VMA Baru: STACK
    // stack_base & USER_STACK_TOP didapat dari Step 5
    VMArea* StackVma = new VMArea();
    if (StackVma) {
        StackVma->Start = stack_base;
        StackVma->End   = USER_STACK_TOP;
        StackVma->Prot  = 0x3; // Read | Write (Stack tidak butuh Exec)
        StackVma->Flags = 0x02 | 0x20; // MAP_PRIVATE | MAP_STACK (jika ada flag stack)
        StackVma->Next  = nullptr;

        // Append ke list (setelah ElfVma)
        if (Current->VMHead) {
            VMArea* Temp = Current->VMHead;
            while (Temp->Next) Temp = Temp->Next;
            Temp->Next = StackVma;
        } else {
            Current->VMHead = StackVma;
        }
    }

    // D. Reset Heap Pointer (brk/mmap start)
    // Kita set start heap di atas area ELF agar tidak menimpa program.
    // Gunakan logika yang sama dengan TaskUserConstructor.
    constexpr U64 USER_HEAP_START = 0x0000000200000000ULL;
    U64 CandidateHeap = (ImageEnd + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    Current->MMapNextAddr = (CandidateHeap > USER_HEAP_START) ? CandidateHeap : USER_HEAP_START;

    // =========================================================================
    // 7. SWITCH CONTEXT (Hardware)
    // =========================================================================

    U64 OldCR3 = Current->CR3;
    
    // Move context to Kernel Stack before switching CR3
    U64 KernelStackTop = (U64)Current->StackBase + Current->StackSize;
    U64 NewContextAddr = KernelStackTop - sizeof(CpuContext_T);
    String::Memcpy((void*)NewContextAddr, CPUContext, sizeof(CpuContext_T));
    CpuContext_T* NewContext = (CpuContext_T*)NewContextAddr;

    // Update Context Registers
    NewContext->rip = Entry;
    NewContext->rsp = InitialRSP;
    NewContext->cs = 0x4B;      // User Code (Ring 3)
    NewContext->ss = 0x43;      // User Data (Ring 3)
    NewContext->rflags = 0x202; // IF = 1
    NewContext->rax = 0;        // Return 0 di userland
    
    // Set up SysV ABI arguments (RDI=argc, RSI=argv, RDX=envp)
    NewContext->rdi = argc_val;
    NewContext->rsi = argv_user_ptr;
    NewContext->rdx = envp_user_ptr; 

    // Commit Hardware Change
    Current->CR3 = NewCR3;
    DoCR3::Load((U64*)NewCR3); // Flush TLB & Switch Page Table

    // Cleanup Old Hardware Resources
    Tasking::FreeUserAddressSpace(OldCR3);

    // Jump to User Mode!
    Context_Restore(NewContext);
}

VOID Sys_Wait(CpuContext_T *CPUContext) {
    Tasking::Task* Current = Tasking::GetCurrentTaskPtr();
    if (!Current) { CPUContext->rax = -1; return; }

    while (true) {
        // 1. AMBIL LOCK DULUAN!
        LOCKRFLAGS irq = Arch::SaveAndDisableInterrupts();

        // 2. Scan ZOMBIE (Di dalam Lock)
        bool HasChildren = false;
        Tasking::Task* ZombieFound = nullptr;

        for (int i = 0; i < MAX_TASK; ++i) {
            Tasking::Task* T = Tasking::TaskArray[i];
            if (T && T->ppid == Current->pid) {
                HasChildren = true;
                if (T->State == Tasking::TaskState::ZOMBIE) {
                    ZombieFound = T;
                    break;
                }
            }
        }

        // KASUS A: Nemu Zombie
        if (ZombieFound) {
            U64 ChildPID = ZombieFound->pid;
            Tasking::DestroyTask(ZombieFound);
            
            Arch::RestoreInterrupts(irq); // UNLOCK sebelum return
            
            CPUContext->rax = ChildPID;
            return;
        }

        // KASUS B: Gak punya anak (ECHILD)
        if (!HasChildren) { 
            Arch::RestoreInterrupts(irq); // UNLOCK
            CPUContext->rax = -1; 
            return; 
        }

        // KASUS C: Anak masih hidup semua -> TIDUR
        // Karena kita pegang LOCK, gak mungkin ada anak yang mati dan ngirim sinyal
        // di sela-sela pengecekan tadi dan baris di bawah ini. AMAN.
        Current->State = Tasking::TaskState::BLOCKED;
        
        Arch::RestoreInterrupts(irq); // UNLOCK (Sekarang aman buat tidur)

        Tasking::SchedulerYield();
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

    // safety check kalo misalkan
    // ada sebuah hacker mencoba mengubah
    // PGID dari proses orang lain yang bukan
    // anaknya sendiri.
    if (TargetTask != Current && TargetTask->ppid != Current->pid) {
        CPUContext->rax = -1; return;
    }

    if(pgid == 0){
        pgid = TargetTask->pid;
    }

    if (TargetTask->PGID != 0 && TargetTask->PGID != pgid) {
        Tasking::UnlinkFromProcGrp(TargetTask);
    }

    // B. Logic Gabung Grup
    if (pgid == TargetTask->pid) {
        // CASE 1: Bikin Grup Baru (Dia jadi Leader)
        TargetTask->PGID = pgid;
        TargetTask->PGIDTaskPtr = nullptr; // Leader awal gak punya temen
    } 
    else {
        // CASE 2: Join Grup Lain
        Tasking::Task* Leader = Tasking::GetTaskPID(pgid);
        
        if (!Leader) {
            CPUContext->rax = -1; // EPERM: Leader gak ketemu
            return;
        }

        // Cek: Apakah Leader beneran Leader? (Opsional, tapi bagus)
        if (Leader->PGID != Leader->pid) {
            // Kita cuma boleh join ke Process yang merupakan Group Leader
            CPUContext->rax = -1; 
            return;
        }

        TargetTask->PGID = pgid;

        // TEKNIK INSERT AT HEAD (O(1))
        // 1. Target nunjuk ke temennya Leader (Anggota 1)
        TargetTask->PGIDTaskPtr = Leader->PGIDTaskPtr;
        
        // 2. Leader nunjuk ke Target
        Leader->PGIDTaskPtr = TargetTask;
    }

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

struct kernel_timespec {
    I64 tv_sec;
    I64 tv_nsec;
};

VOID Sys_SleepNs(CpuContext_T *CPUContext){
    UPTR req_addr = CPUContext->rdi;
    __MAYBE_UNUSED UPTR rem_addr = CPUContext->rsi;

    if(req_addr == 0){ CPUContext->rax = -14; return; }

    Tasking::Task* Current = Tasking::GetCurrentTaskPtr();
    if (!Current) { CPUContext->rax = -1; return; }

    // --- SAFETY FIRST: CopyFromUser ---
    U64* UserPML4 = HHDM_PhysToVirt(Current->CR3);
    kernel_timespec kreq;

    if (!PageAlloc::CopyFromUser(UserPML4, &kreq, (void*)req_addr, sizeof(kernel_timespec))) {
        CPUContext->rax = -14; // EFAULT
        return;
    }

    I64 Seconds = kreq.tv_sec;
    I64 Nanoseconds = kreq.tv_nsec;

    if(Nanoseconds < 0 || Nanoseconds >= 1000000000){ CPUContext->rax = -22; return; }

    U64 TargetFreq = ACPI::Timer::LapicHz; 
    U64 TicksFromSec = (U64)Seconds * TargetFreq;
    U64 TicksFromNs = ((U64)Nanoseconds * TargetFreq) / 1000000000ULL;
    U64 TicksToSleep = TicksFromSec + TicksFromNs;

    if (TicksToSleep == 0 && (Seconds > 0 || Nanoseconds > 0)) {
        TicksToSleep = 1;
    }

    Current->BlockReason |= TASK_SLEEPING;
    Current->SleepTick = ACPI::Timer::LapicTicks + TicksToSleep; 
    Current->State = Tasking::TaskState::BLOCKED;   

    // [WAJIB DITAMBAH]
    // Masukin ke Linked List O(1) biar dicek sama CheckSleepingTasks()
    Tasking::AddToSleepList(Current); 

    Tasking::SchedulerYield(); 

    CPUContext->rax = 0;
}

VOID Sys_Signal(CpuContext_T *CPUContext){
    U64 signum = CPUContext->rdi;
    U64 handler = CPUContext->rsi;

    Tasking::Task* Current = Tasking::GetCurrentTaskPtr();
    if (!Current) { CPUContext->rax = -1; return; }

    if (signum >= 32) {
        CPUContext->rax = -1; // Invalid signal number
        return;
    }

    // SIGKILL (9) cannot be caught or ignored
    if (signum == 9) {
        CPUContext->rax = -1;
        return;
    }

    // Return old handler
    U64 old_handler = Current->SignalHandlers[signum];
    Current->SignalHandlers[signum] = handler;
    
    CPUContext->rax = old_handler;
}

struct KernelSigAction {
    U64 sa_handler;
    U64 sa_flags;
    U64 sa_restorer;
    U64 sa_mask;
};

VOID Sys_RtSigAction(CpuContext_T *CPUContext) {
    // Arguments:
    // RDI: int sig
    // RSI: const struct sigaction *act
    // RDX: struct sigaction *oact
    // R10: size_t sigsetsize

    I32 sig = (I32)CPUContext->rdi;
    U64 act_addr = CPUContext->rsi;
    U64 oact_addr = CPUContext->rdx;
    __MAYBE_UNUSED U64 sigsetsize = CPUContext->r10;

    Tasking::Task* Current = Tasking::GetCurrentTaskPtr();
    if (!Current) { CPUContext->rax = -1; return; }

    if (sig >= 32 || sig <= 0) {
        CPUContext->rax = -22; // EINVAL
        return;
    }
    
    // SIGKILL (9) and SIGSTOP (19) cannot be caught
    if (sig == 9 || sig == 19) {
        CPUContext->rax = -22; // EINVAL
        return;
    }

    /*if (sigsetsize != 8) {
        //             (unsigned long long)sigsetsize);
        C We only support 64-bit signal mask for now (standard x86_64)
        Printk::Write(Printk::Level::LOG_DEBUG,
                      "Sys_RtSigAction: invalid sigsetsize %llu\n",
         PUContext->rax = -22; // EINVAL
        return;
    }*/

    // Handle Old Action (oact)
    if (oact_addr) {
        KernelSigAction ksa_old;
        String::Memset(&ksa_old, 0, sizeof(KernelSigAction));
        ksa_old.sa_handler = Current->SignalHandlers[sig];
        // We don't store flags/mask yet, so return 0 for them
        
        U64* UserPML4 = HHDM_PhysToVirt(Current->CR3);
        if (!PageAlloc::CopyToUser(UserPML4, (void*)oact_addr, &ksa_old, sizeof(KernelSigAction))) {
            Printk::Write(Printk::Level::LOG_DEBUG,
                          "Sys_RtSigAction: CopyToUser failed for oact_addr %p\n",
                          (void*)oact_addr);
             CPUContext->rax = -14; // EFAULT
             return;
        }
    }

    // Handle New Action (act)
    if (act_addr) {
        KernelSigAction ksa_new;
        U64* UserPML4 = HHDM_PhysToVirt(Current->CR3);
        if (!PageAlloc::CopyFromUser(UserPML4, &ksa_new, (void*)act_addr, sizeof(KernelSigAction))) {
            Printk::Write(Printk::Level::LOG_DEBUG,
                          "Sys_RtSigAction: CopyFromUser failed for act_addr %p\n",
                          (void*)act_addr);
             CPUContext->rax = -14; // EFAULT
             return;
        }

        Current->SignalHandlers[sig] = ksa_new.sa_handler;
        // TODO: Store flags and restorer
    }

    Printk::Write(Printk::Level::LOG_DEBUG,
                  "PID %d set signal handler for signal %d to %p\n",
                  Current->pid, sig,
                  (void*)Current->SignalHandlers[sig]);
    CPUContext->rax = 0;
}

VOID Sys_Poll(CpuContext_T *CPUContext){
    U64 fds_addr = CPUContext->rdi;
    U64 nfds     = CPUContext->rsi;
    I64 timeout  = (I64)CPUContext->rdx;

    Tasking::Task *Current = Tasking::GetCurrentTaskPtr();
    if(!Current){
        CPUContext->rax = -10;
        return;
    }

    if(nfds > 64){
        CPUContext->rax = -22; // EINVAL (Terlalu banyak) 
        return;
    }

    kernel_pollfd kfds[64];

    U64 *UserPML4 = HHDM_PhysToVirt(Current->CR3);
    if(!PageAlloc::CopyFromUser(UserPML4, kfds, (void*)fds_addr, nfds * sizeof(kernel_pollfd))){
        CPUContext->rax = -14;
        return;
    }

    U64 StartTick = ACPI::Timer::LapicTicks;
    U64 TimeoutTicks = (timeout >= 0) ? ACPI::Timer::MillisecondsToTicks(timeout) : 0;
    U64 Deadline = StartTick + TimeoutTicks;

    INTN ReadyCount = 0;

    while(TRUE){
        ReadyCount = 0;

        for(U64 i = 0; i < nfds; i++){
            kfds[i].revents = 0;

            short mask = Tasking::CheckFileDesc(kfds[i].fd, kfds[i].events);

            if(mask){
                kfds[i].revents = mask;
                ReadyCount++;
            }
        }

        if (ReadyCount > 0) break;

        // 2. Timeout habis? (Dan bukan infinite wait)
        if (timeout >= 0 && ACPI::Timer::LapicTicks >= Deadline) break;
        
        // 3. User minta Non-Blocking (Timeout 0)? Return immediately
        if (timeout == 0) break;

        if (timeout < 0) {
            Tasking::SchedulerYield(); 
            continue; 
        }

        Current->BlockReason |= TASK_SLEEPING;

        Current->SleepTick = 1; // 1 Tick relatif pendek
        Current->State = Tasking::TaskState::BLOCKED;

        Tasking::AddToSleepList(Current);    
        Tasking::SchedulerYield();
    }

    if (!PageAlloc::CopyToUser(UserPML4, (void*)fds_addr, kfds, nfds * sizeof(kernel_pollfd))) {
        CPUContext->rax = -14; // EFAULT
        return;
    }

    CPUContext->rax = ReadyCount;
}

VOID Sys_Yield(CpuContext_T *CPUContext){
    Tasking::SchedulerYield();
    CPUContext->rax = 0;
}

// ===================================
// Custom syscall
// ===================================

// tidurkan task berdasarkan ms
VOID SysSleepMS(CpuContext_T *CPUContext){
    U64 ms = CPUContext->rdi;

    Tasking::Task* Current = Tasking::GetCurrentTaskPtr();
    if (!Current) { CPUContext->rax = -1; return; }

    // Konversi MS ke Ticks
    // Pastikan fungsi ini nerima MS. Kalau dia nerima Microseconds, harus dikali 1000.
    // Asumsi: ACPI::Timer::MillisecondsToTicks ada, atau pake rumus manual:
    // U64 ticks = (ms * ACPI::Timer::LapicHz) / 1000;
    
    // Anggap lu pake helper yang bener:
    U64 ticks = (ms * ACPI::Timer::LapicHz) / 1000; 
    
    if (ticks == 0 && ms > 0) {
        ticks = 1; 
    }

    Current->BlockReason |= TASK_SLEEPING;
    Current->SleepTick = ACPI::Timer::LapicTicks + ticks;
    Current->State = Tasking::TaskState::BLOCKED;

    // [WAJIB DITAMBAH]
    Tasking::AddToSleepList(Current);

    Tasking::SchedulerYield();
}

VOID Sys_SetAppPerm(CpuContext_T *CPUContext){
    U32 Permission = (U32)CPUContext->rdi;

    if(!Permission){
        CPUContext->rax = (U64)-10;
        return;
    }

    Tasking::Task *Current = Tasking::GetCurrentTaskPtr();
    if(!Current){
        CPUContext->rax = (U64)-4;
        return;
    }

    Tasking::SettingAppPerm(Current, Permission);

    CPUContext->rax = 0;
}

// |=====================================|
// |                                     |
// |         End Custom Syscalls         |
// |                                     |
// |=====================================|

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

VOID Sys_GetClockTime(CpuContext_T *CPUContext){
    U64 WhatToTake = CPUContext->rdi;
    UPTR TPAddress = CPUContext->rsi;

    if(!TPAddress) {
        CPUContext->rax = -ROS_INVALID;
        return;
    }

    Tasking::Task *Current = Tasking::GetCurrentTaskPtr();
    if(!Current){
        CPUContext->rax = -ROS_BUSY;
        return;
    }

    // Prepare kernel-local timespec to copy to user
    struct kernel_timespec kt;
    kt.tv_sec = 0;
    kt.tv_nsec = 0;

    if (WhatToTake == CLOCK_MONOTONIC) {
        U64 TSCNow = Arch::ASM::RdTSC();
        U64 DeltaTSC = TSCNow - ACPI::Timer::BootTSC;
        U64 Freq = ACPI::Timer::TSCFrequencyHz;
        if (Freq == 0) Freq = 1; // avoid div-by-zero (shouldn't happen)

        U64 Sec = DeltaTSC / Freq;
        U64 RemainderTicks = DeltaTSC % Freq;
        U64 Nanoseconds = (RemainderTicks * 1000000000ULL) / Freq;

        kt.tv_sec = (I64)Sec;
        kt.tv_nsec = (I64)Nanoseconds;
    }
    else if (WhatToTake == CLOCK_REALTIME) {
        Printk::Write(Printk::Level::LOG_ALERT, "Unimplemented.\n");
        CPUContext->rax = -ROS_NOTFOUND;
        return;
    } else {
        CPUContext->rax = -ROS_INVALID;
        return;
    }

    // Copy result back to userland
    U64* UserPML4 = HHDM_PhysToVirt(Current->CR3);
    if (!PageAlloc::CopyToUser(UserPML4, (void*)TPAddress, &kt, sizeof(kt))) {
        CPUContext->rax = -14; // EFAULT
        return;
    }

    CPUContext->rax = 0;
}

VOID Sys_SetAppPerm(CpuContext_T *CPUContext);