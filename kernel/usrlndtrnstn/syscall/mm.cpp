#include <rosval.h>
#include <mm.hpp>
#include "bootinfo.h"
#include "cpu_context.hpp"
#include "ros_linux/mmprotocol.hpp"
#include "string.hpp"
#include "syscall/sysarg.hpp"
#include "syscall/mm.hpp"
#include <task.hpp>
#include <filesystem/filesystem.hpp>
#include <../kernel/mm/shm/shm.hpp>

VOID Sys_Brk(CpuContext_T *CPUContext){
    using namespace Tasking;

    U64 NewBrk = CPUContext->rdi;
    Task *Current = GetCurrentTaskPtr();

    if(!Current){
        CPUContext->rax = -ROS_BUSY;
        return;
    }

    if (NewBrk == 0) {
        CPUContext->rax = Current->MMapNextAddr;
        return;
    }

    U64 CurrentBrk = Current->MMapNextAddr;
    U64 NewBrkAligned = (NewBrk + 0xFFF) & ~0xFFF;

    if(NewBrkAligned > CurrentBrk){
        U64 SizeNeeded = NewBrkAligned - CurrentBrk;
        U64 PagesNeeded = SizeNeeded / PAGE_SIZE;

        UPTR PhysPtr = PageAlloc::PhysicalAllocPages(PagesNeeded);
        if(!PhysPtr){ CPUContext->rax = -ROS_NOMEM; return; }

        VOID *VirtPtr = HHDM_PhysToVirt(PhysPtr);
        String::Memset(VirtPtr, 0, PagesNeeded * PAGE_SIZE); // Zero out the new memory for safety

        U64 *UserPML4 = HHDM_PhysToVirt(Current->CR3);

        BOOL Success = PageAlloc::MapPages(UserPML4, PhysPtr, CurrentBrk, PagesNeeded, PAGE_PRESENT | PAGE_RW | PAGE_USER);

        if(!Success){
            PageAlloc::PhysicalFreePages(PhysPtr, PagesNeeded);
            CPUContext->rax = -1;
            return;
        }

        Current->MMapNextAddr = NewBrkAligned;
    }

    // TODO: Handle shrinking
    CPUContext->rax = Current->MMapNextAddr;
}

// MMAP Utility
STATIC CONSTANT U64 USER_MAX_ADDR = 0x00007FFFFFFFF000ULL;

STATIC Tasking::VMArea *SplitVMA(Tasking::Task *TSK, Tasking::VMArea *Target, U64 SplitAddr){
    if (!Target || SplitAddr <= Target->Start || SplitAddr >= Target->End) return nullptr;

    Tasking::VMArea *NewRight = new Tasking::VMArea();
    if (!NewRight) return nullptr; // OOM

    *NewRight = *Target;

    Target->End = SplitAddr;       // Kiri: Start s/d SplitAddr
    NewRight->Start = SplitAddr;   // Kanan: SplitAddr s/d End lama

    if(NewRight->BackingFile){
        NewRight->FileOffset += (SplitAddr - Target->Start);
        NewRight->BackingFile->RefCount++; // Kita duplikat referensi ke file
    }

    NewRight->Next = Target->Next;
    Target->Next = NewRight;

    return NewRight;
}

STATIC NORESULTFUNC UpdatePageTableProt(Tasking::Task *tsk, U64 Start, U64 Len, U64 Prot){
    U64 PteFlags = PAGE_USER;
    
    // Kalau ada permission APAPUN, Page harus PRESENT.
    // Kecuali PROT_NONE (0), maka Present = 0.
    if (Prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) {
        PteFlags |= PAGE_PRESENT;
    }

    if(Prot & PROT_WRITE) PteFlags |= PAGE_RW;
    
    // NX Bit (No Execute).
    // Note: Di x86, kalau mau ENABLE execute, NX bit harus 0.
    // Kalau mau DISABLE execute, NX bit harus 1.
    // Jadi logic lo sebelumnya (&= ~PAGE_NX) itu bener buat enable exec.
    // Tapi pastikan default PteFlags lo set NX bit nyala (disable) kalau fitur ini aktif.
    if(Prot & PROT_EXEC) PteFlags &= ~PAGE_NX; 
    else PteFlags |= PAGE_NX; // (Optional: Enforce NX if supported)

    U64 Pages = (Len + PAGE_SIZE - 1) / PAGE_SIZE;

    for (U64 i = 0; i < Pages; i++) {
        U64 VirtAddr = Start + (i * PAGE_SIZE);
        
        // Contoh Pseudo implementation:
        if(!PageAlloc::SetFlags((U64*)tsk->CR3, VirtAddr, PteFlags)){
            return;
        }
        Arch::Invlpg(VirtAddr); 
    }
}

STATIC BOOL IsValidUserRange(U64 Start, U64 Len) {
    if (Start == 0) return FALSE; // Null pointer guard (opsional)
    
    // Cek Overflow
    if (Start + Len < Start) return FALSE; 

    // Cek Batas Atas (Jangan sampai masuk area Kernel)
    if ((Start + Len) > USER_MAX_ADDR) return FALSE;

    return TRUE;
}

STATIC BOOL TryMergeNext(Tasking::VMArea *Current) {
    if (!Current || !Current->Next) return FALSE;

    Tasking::VMArea *Next = Current->Next;

    // Syarat 1: Address harus nyambung (Contiguous)
    if (Current->End != Next->Start) return FALSE;

    // Syarat 2: Permission & Flags harus sama persis
    if (Current->Prot != Next->Prot) return FALSE;
    if (Current->Flags != Next->Flags) return FALSE;

    // Syarat 3: File Backing harus konsisten
    if (Current->BackingFile != Next->BackingFile) return FALSE;

    // Syarat 4: Jika ada file, offsetnya harus nyambung
    if (Current->BackingFile) {
        U64 SizeCurrent = Current->End - Current->Start;
        if (Current->FileOffset + SizeCurrent != Next->FileOffset) {
            return FALSE; // File offset lompat, tidak bisa merge
        }
    }

    // --- LAKUKAN MERGE ---
    // Perpanjang Current
    Current->End = Next->End;
    
    // Bypass Next
    Current->Next = Next->Next;

    // Hapus struct Next (Hemat memori kernel)
    delete Next; 

    return TRUE;
}

STATIC U64 FindFreeRegion(Tasking::Task *Tsk, U64 Length){
    U64 StartAddr = Tsk->MMapNextAddr;
    if(StartAddr & 0xFFF) StartAddr = (StartAddr + 0xFFF) & ~0xFFF;

    CONSTANT U64 USER_HEAP_START = 0x0000000200000000ULL;
    if(StartAddr < USER_HEAP_START) StartAddr = USER_HEAP_START;

    using namespace Tasking;
    __MAYBE_UNUSED VMArea *Current = Tsk->VMHead;

    U64 SearchLimit = USER_MAX_ADDR - Length;

    while(TRUE){
        // SAFETY GUARD: Kalau sudah mentok ke area kernel/limit, stop.
        if (StartAddr > SearchLimit) {
            return 0; // Gagal nemu tempat (Out of Memory/Address Space)
        }

        U64 EndAddr = StartAddr + Length;
        BOOL Collision = FALSE;

        VMArea *Iterator = Tsk->VMHead;
        while(Iterator){
            // Optimasi: Kalau StartAddr sudah lewat jauh dari Iterator, skip (kalo list sorted)
            // Tapi karena list mungkin acak saat insert manual, full scan gapapa untuk sekarang.
            
            if (StartAddr < Iterator->End && EndAddr > Iterator->Start) {
                // Tabrakan! Lompat ke akhir VMA pengganggu
                StartAddr = Iterator->End;
                if (StartAddr & 0xFFF) StartAddr = (StartAddr + 0xFFF) & ~0xFFF;
                Collision = TRUE;
                break; 
            }
            Iterator = Iterator->Next;
        }

        if(!Collision){
            return StartAddr;
        }
    }
}

static VOID InsertVmaSorted(Tasking::Task *Task, Tasking::VMArea *NewVma) {
    using namespace Tasking;

    // Kasus 1: List Kosong atau NewVma ada di paling depan
    if (!Task->VMHead || NewVma->Start < Task->VMHead->Start) {
        NewVma->Next = Task->VMHead;
        Task->VMHead = NewVma;
        
        // Coba merge dengan depannya (sekarang jadi Next dari NewVma)
        TryMergeNext(NewVma);
        return;
    }

    // Kasus 2: Cari posisi di tengah/akhir
    VMArea *Iter = Task->VMHead;
    while (Iter->Next && Iter->Next->Start < NewVma->Start) {
        Iter = Iter->Next;
    }

    // Sisipkan setelah 'Iter'
    NewVma->Next = Iter->Next;
    Iter->Next = NewVma;

    // Coba Merge Kanan: NewVma dengan Next-nya
    TryMergeNext(NewVma);

    // Coba Merge Kiri: Iter (Prev) dengan NewVma
    // Perhatikan: Jika Merge Kiri sukses, NewVma akan terhapus (dimakan Iter)
    // dan Iter akan memanjang. Itu valid.
    TryMergeNext(Iter);
}

STATIC VOID UnmapRange(Tasking::Task *Task, U64 Start, U64 Size, BOOL FreePhysical) {
    U64 *UserPML4 = HHDM_PhysToVirt(Task->CR3);
    U64 Pages = (Size + PAGE_SIZE - 1) / PAGE_SIZE;

    BOOL UseInvlpg = (Pages < 512);
    
    for(U64 i = 0; i < Pages; i++) {
        U64 VirtAddr = Start + (i * PAGE_SIZE);
        
        // Dapatkan PhysAddr sebelum di unmap untuk di-free (jika anonymous)
        UPTR PhysAddr = PageAlloc::GetPhysicalAddress(UserPML4, VirtAddr);
        
        if (PhysAddr) {
            PageAlloc::UnMapPages(UserPML4, VirtAddr);

            if(UseInvlpg) Arch::Invlpg(VirtAddr);
            
            // HATI-HATI: Jangan free fisik jika ini SHM atau Hardware Mapped!
            // Logic ini harus dikontrol oleh caller (Sys_Munmap) via parameter FreePhysical
            if (FreePhysical) {
                PageAlloc::PhysicalFreePages(PhysAddr, 1);
            }
        }
    }

    if(!UseInvlpg) DoCR3::Load((U64*)Task->CR3);
}

static VOID FreeVmaStruct(Tasking::VMArea *Vma) {
    if (Vma->BackingFile) {
        // Turunkan RefCount. Jika 0, VFSManager::Close akan handle cleanup
        // Asumsi: Kamu punya fungsi VFSManager::CloseFileStruct(File*) 
        // atau manual decrement.
        Vma->BackingFile->RefCount--;
        
        // Jika RefCount == 0 dan FD sudah tidak ada yang pegang, 
        // idealnya panggil logic cleanup file driver.
        // Untuk sekarang, decrement saja sudah cukup mencegah dangling pointer.
    }
    delete Vma;
}

VOID Sys_Munmap(CpuContext_T *CPUContext){
    using namespace Tasking;
    U64 Addr = CPUContext->rdi;
    U64 Length = CPUContext->rsi;

    if (Length == 0) { CPUContext->rax = -ROS_INVALID; return; }
    if (Addr & 0xFFF) { CPUContext->rax = -ROS_INVALID; return; }
    
    U64 AlignedLength = (Length + 0xFFF) & ~0xFFF;

    if (!IsValidUserRange(Addr, AlignedLength)) {
        CPUContext->rax = -ROS_INVALID; 
        return;
    }

    Task *Current = GetCurrentTaskPtr();
    if(!Current || Length == 0){
        CPUContext->rax = -ROS_INVALID;
        return;
    }

    if(Addr & 0xFFF){
        CPUContext->rax = -ROS_INVALID;
        return;
    }
    Length = (Length + 0xFFF) & ~0xFFF;
    U64 EndAddr = Addr + Length;

    VMArea *Prev = nullptr;
    VMArea *Curr = Current->VMHead;

    while(Curr){
        if(Curr->End <= Addr || Curr->Start >= EndAddr){
            Prev = Curr;
            Curr = Curr->Next;
            continue;
        }

        if(Curr->Start >= Addr && Curr->End <= EndAddr){
            BOOL IsAnon = (Curr->BackingFile == nullptr);
            UnmapRange(Current, Curr->Start, Curr->End - Curr->Start, IsAnon);

            VMArea *ToFree = Curr;
            if(Prev) Prev->Next = Curr->Next;
            else Current->VMHead = Curr->Next;

            Curr = Curr->Next; // Lanjut iterasi
            
            // Free struct VMA (Pake KFree atau operator delete)
            FreeVmaStruct(ToFree);
            continue;
        }

        if(Curr->Start < Addr){
            if(Curr->End > EndAddr){
                VMArea *RightChunk = new VMArea();
                *RightChunk = *Curr;
                RightChunk->Start = EndAddr;
                RightChunk->FileOffset += (EndAddr - Curr->Start);

                RightChunk->Next = Curr->Next;
                Curr->Next = RightChunk;

                BOOL IsAnon = (Curr->BackingFile == nullptr);
                UnmapRange(Current, Addr, EndAddr - Addr, IsAnon);

                Curr->End = Addr;
                Prev = Curr;
                Curr = Curr->Next;
                continue;
            } else {
                // TAIL CUT: Unmap memakan ekor VMA
                // Unmap pages
                BOOL IsAnon = (Curr->BackingFile == nullptr);
                UnmapRange(Current, Addr, Curr->End - Addr, IsAnon);
                
                // Shrink VMA
                Curr->End = Addr;
            }
        }
        else if(Curr->End > EndAddr){
            BOOL IsAnon = (Curr->BackingFile == nullptr);
            UnmapRange(Current, Curr->Start, EndAddr - Curr->Start, IsAnon);
            
            // Adjust Offset file jika ada
            if (Curr->BackingFile) {
                Curr->FileOffset += (EndAddr - Curr->Start);
            }
            // Shrink
            Curr->Start = EndAddr;
        }
        Prev = Curr;
        Curr = Curr->Next;
    }

    CPUContext->rax = 0;
}

VOID Sys_MMap(CpuContext_T *CPUContext){
    using namespace Tasking;

    U64 Addr = CPUContext->rdi;
    U64 Length = CPUContext->rsi;
    U64 Prot = CPUContext->rdx;
    U64 Flags = CPUContext->r10;
    U64 FD = CPUContext->r8;
    U64 Offset = CPUContext->r9;

    if(Length == 0){
        CPUContext->rax = -ROS_INVALID;
        return;
    }

    U64 AlignedLength = (Length + 0xFFF) & ~0xFFF;

    if (Addr != 0) {
        // Harus align page
        if (Addr & 0xFFF) { CPUContext->rax = -ROS_INVALID; return; }
        
        // Cek Security (Validasi Address)
        if (!IsValidUserRange(Addr, AlignedLength)) {
            CPUContext->rax = -ROS_INVALID; // Atau -ROS_EFAULT
            return;
        }
    }

    Task *Current = GetCurrentTaskPtr();
    if(!Current){
        CPUContext->rax = -ROS_BUSY;
        return;
    }

    U64 VirtualAddr = Addr;
    if(VirtualAddr == 0){
        VirtualAddr = FindFreeRegion(Current, Length);
        if(VirtualAddr == 0){
            CPUContext->rax = -ROS_NOMEM;
            return;
        }
        if (!IsValidUserRange(VirtualAddr, AlignedLength)) {
            CPUContext->rax = -ROS_NOMEM;
            return;
        }
    } 
    else{
        if (VirtualAddr & 0xFFF) { // Harus page aligned
             CPUContext->rax = -ROS_INVALID;
             return;
        }

        VMArea *Iter = Current->VMHead;
        U64 RequestEnd = VirtualAddr + AlignedLength;
        while(Iter) {
            // Cek Intersection
            if (VirtualAddr < Iter->End && RequestEnd > Iter->Start) {
                // Tabrakan dengan VMA yang sudah ada!
                // Di Linux asli, behavior MAP_FIXED akan menimpa (unmap otomatis).
                // Tapi kalau tanpa flag MAP_FIXED, ini harusnya return error.
                // Untuk safety OS-mu sekarang: Error aja.
                CPUContext->rax = -ROS_INVALID; // Atau -EEXIST
                return; 
            }
            Iter = Iter->Next;
        }
    }

    U64 Pages = (Length + PAGE_SIZE - 1) / PAGE_SIZE;
    __MAYBE_UNUSED UPTR PhysToMap = 0;
    File *BackingFile = nullptr;

    if(Flags & 0x20){ // MAP_ANONYMOUS
        
    } else {
        // MAP FILE / DEVICE
        if(FD >= MAX_FILE_IN_PROCESS || !Current->FDTable[FD]){
            CPUContext->rax = -ROS_INVALID;
            return;
        }

        BackingFile = Current->FDTable[FD];
        if(BackingFile->type == FT_SHM){
            ShmRegion *Region = (ShmRegion*)BackingFile->PrivateData;
            // SHM offset handling is tricky, assuming 0 for now or handling offset
            if (Offset + (Pages*PAGE_SIZE) > Region->SizeInPages * PAGE_SIZE) {
                CPUContext->rax = -ROS_INVALID; 
                return;
            }
            PhysToMap = Region->PhysAddr + Offset; // Simplifikasi fisik kontigu
        }
        else if(String::Strcmp((const CHAR8*)BackingFile->FileName, (const CHAR8*)"/dev/fb0") == 0){
            const BootInfo *Bi = BootInfoGet();
            if(!Bi){ CPUContext->rax = -10; return; }
            PhysToMap = Bi->framebuffer.address;
        } else {
            BackingFile->RefCount++;
        }
    }

    VMArea *NewVma = new VMArea();
    if(!NewVma){
        if (BackingFile && !(BackingFile->type == FT_SHM) /* logic simple check */) {
             // Sebaiknya cek apakah tadi kita increment refcount
             // Tapi untuk regular file:
             BackingFile->RefCount--;
        }
        CPUContext->rax = -ROS_NOMEM; 
        return;
    }

    NewVma->Start = VirtualAddr;
    NewVma->End = VirtualAddr + (Pages * PAGE_SIZE);
    NewVma->Prot = Prot;
    NewVma->Flags = Flags;
    NewVma->BackingFile = BackingFile; // Increment refcount in real impl
    NewVma->FileOffset = Offset;

    InsertVmaSorted(Current, NewVma);

    if (NewVma->End > Current->MMapNextAddr) {
        Current->MMapNextAddr = NewVma->End;
    }
    CPUContext->rax = VirtualAddr;
}

VOID Sys_Mprotect(CpuContext_T *ctx){
    using namespace Tasking;

    U64 Addr = CATCHARG1(ctx);
    U64 Len = CATCHARG2(ctx);
    U64 Prot = CATCHARG3(ctx);

    if (Len == 0) { RETVAL(ctx) = 0; return; } // Sukses tapi gak ngapa2in
    if (Addr & 0xFFF) { RETVAL(ctx) = -ROS_INVALID; return; } // Harus page aligned

    U64 AlignedLen = (Len + 0xFFF) & ~0xFFF;
    if (!IsValidUserRange(Addr, AlignedLen)) { RETVAL(ctx) = -ROS_INVALID; return; }

    Task *Current = GetCurrentTaskPtr();
    U64 EndAddr = Addr + AlignedLen;
    U64 ScanAddr = Addr;

    while (ScanAddr < EndAddr) {
        
        // Cari VMA yang mengandung ScanAddr
        VMArea *Iter = Current->VMHead;
        VMArea *Found = nullptr;
        while (Iter) {
            if (ScanAddr >= Iter->Start && ScanAddr < Iter->End) {
                Found = Iter;
                break;
            }
            Iter = Iter->Next;
        }

        // Kalau ada gap (memory bolong), mprotect harus fail (Standard Linux)
        if (!Found) {
            RETVAL(ctx)= -ROS_NOMEM; 
            return;
        }

        // 3. Logic Splitting (Memotong VMA agar pas dengan request)

        // Case A: Request mulai di tengah VMA ini
        // [VMA Start .... ScanAddr .... VMA End] -> Potong di ScanAddr
        if (ScanAddr > Found->Start) {
            VMArea *RightPart = SplitVMA(Current, Found, ScanAddr);
            if (!RightPart) { RETVAL(ctx)= -ROS_NOMEM; return; }
            
            // Sekarang 'Found' adalah sisi kiri (yg gak diubah).
            // Kita lanjut loop biar logic bawah nangkep 'RightPart' sebagai target.
            continue; 
        }

        // Case B: Request selesai sebelum VMA ini berakhir
        // [ScanAddr .... EndAddr .... VMA End] -> Potong di EndAddr
        if (EndAddr < Found->End) {
            if (!SplitVMA(Current, Found, EndAddr)) { 
                RETVAL(ctx) = -ROS_NOMEM; return; 
            }
            // Sekarang 'Found' berakhir pas di EndAddr. Aman.
        }

        // 4. Update Permission (Software)
        Found->Prot = Prot;

        // 5. Update Permission (Hardware/TLB)
        // Hitung panjang yang efektif di VMA ini saja
        U64 EffectiveLen = Found->End - Found->Start;
        UpdatePageTableProt(Current, Found->Start, EffectiveLen, Prot);

        // 6. MERGING (Re-use code lo!)
        // Permission VMA ini mungkin sekarang sama dengan VMA Kanan (Next).
        // Coba merge Found dengan Found->Next
        TryMergeNext(Found);

        // HATI-HATI: Kita juga harus cek merge dengan VMA Kiri (Prev).
        // Tapi karena linked list lo singly linked, kita gak punya pointer Prev di sini dengan mudah.
        // Opsi: Loop ulang cari prev, atau abaikan (fragmentasi dikit gapapa).
        // Ide Hack: Karena loop kita maju terus (ScanAddr naik), merge ke kanan (TryMergeNext)
        // biasanya cukup untuk menyatukan potongan yang kita buat sendiri.
        
        ScanAddr = Found->End;
    }

    // Optional: Full scan merge pass kalau mau rapi banget (karena keterbatasan singly linked list)
    // VMArea *Cleanup = Current->VMHead;
    // while(Cleanup) { TryMergeNext(Cleanup); Cleanup = Cleanup->Next; }

    RETVAL(ctx) = 0;
}

VOID Sys_ShmOpen(CpuContext_T *Ctx){
    U64 name = Ctx->rdi;
    U64 Size = Ctx->rsi;
    U64 Flags = Ctx->rdx;

    Tasking::Task *Current = Tasking::GetCurrentTaskPtr();
    if(!Current){
        Printk::Write(Printk::Level::LOG_DERR, "Sys_ShmOpen: Can't get current task.\n");
        Ctx->rax = -ROS_BUSY;
        return;
    }

    U64 *UserPML4 = HHDM_PhysToVirt(Current->CR3);

    CHAR8 Name[128];
    if(!PageAlloc::CopyFromUser(UserPML4, &Name, (VOID*)name, sizeof(Name))){
        Printk::Write(Printk::Level::LOG_DERR, "Sys_ShmOpen: Copy to user failed.\n");
        Ctx->rax = -ROS_NOMEM;
        return;
    }

    BOOL Create = (Flags & O_CREAT) != 0;

    ShmRegion *Region = SharedMemoryManager::Open(Name, Size, Create);

    if(!Region){
        Printk::Write(Printk::Level::LOG_DERR, "Sys_ShmOpen: No Region\n");
        Ctx->rax = -ROS_NOMEM;
        return;
    }

    File *f = new File();
    f->type = FileType::FT_SHM;
    f->CurrentPosition = 0;
    f->RefCount = 1;
    f->IsDirectory = FALSE;

    f->Node = Region->RegionInode;
    f->PrivateData = (VOID*)Region;
    String::Strcpy(f->FileName, Region->Name);
    int fd = -1; // Init dengan -1

    for(INTN i = 0; i < MAX_FILE_IN_PROCESS; i++){
        if(Current->FDTable[i] == nullptr){
            Current->FDTable[i] = f;
            fd = i;
            break;
        }
    }

    // Cek apakah dapet slot?
    if (fd == -1) {
        // Try to cleanup stale entries (RefCount <= 0) then retry once
        for(INTN i = 0; i < MAX_FILE_IN_PROCESS; i++){
            File *ff = Current->FDTable[i];
            if(ff && ff->RefCount <= 0){
                VFSManager::Close(ff);
                Current->FDTable[i] = nullptr;
            }
        }

        // Retry allocation
        for(INTN i = 0; i < MAX_FILE_IN_PROCESS; i++){
            if(Current->FDTable[i] == nullptr){
                Current->FDTable[i] = f;
                fd = i;
                break;
            }
        }

        if (fd == -1) {
            // As a last resort try to close descriptors owned ONLY by this task (RefCount==1)
            for(INTN i = 3; i < MAX_FILE_IN_PROCESS; i++){
                File *ff = Current->FDTable[i];
                if(ff && ff->RefCount == 1){
                    Printk::Write(Printk::Level::LOG_DEBUG, "Sys_ShmOpen: Closing local-only FD %d to free slot\n", i);
                    VFSManager::Close(ff);
                    Current->FDTable[i] = nullptr;
                    // take this slot for our new file
                    Current->FDTable[i] = f;
                    fd = i;
                    break;
                }
            }

            // If still not found, log FD table for diagnosis
            // Log FD table for diagnosis
            Printk::Write(Printk::Level::LOG_DERR, "Sys_ShmOpen: No FD Slot\n");
            for(INTN i = 0; i < MAX_FILE_IN_PROCESS; i++){
                File *ff = Current->FDTable[i];
                if(ff){
                    Printk::Write(Printk::Level::LOG_DERR, " FD[%d]=%p name='%s' type=%d ref=%d\n", i, (void*)ff, ff->FileName, (int)ff->type, ff->RefCount);
                } else {
                    Printk::Write(Printk::Level::LOG_DERR, " FD[%d]=<empty>\n", i);
                }
            }

            if (fd == -1) {
                delete f; // Hapus object biar gak leak
                Ctx->rax = -ROS_NOMEM; // Atau error code lain (Too Many Open Files)
                return;
            }
        }
    }

    Ctx->rax = fd;
}