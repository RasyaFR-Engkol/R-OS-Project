#include "bootinfo.h"
#include "rossys.hpp"
#define PRINTK_MODULE_NAME "ISRServ"
#include "../log/printk/printk.hpp"
#include <rosval.h>
#include <string.hpp>
#include <mm.hpp>
#include <../kernel/filesys/vfs/vfs.hpp>
#include <../kernel/mm/shm/shm.hpp>

static BOOL HandleUserPageFault(Tasking::Task *Current, U64 FaultAddr, U64 ErrorCode) {
    using namespace Tasking;
    // Serial::Printf("PFHandler: Handling %p...\n", FaultAddr); 

    U64 PageFaultAddrAligned = FaultAddr & ~0xFFF;
    U64 *UserPML4 = HHDM_PhysToVirt(Current->CR3);

    // 1. Cari VMA
    VMArea *Vma = Current->VMHead;
    BOOL Found = FALSE;
    while (Vma) {
        if (FaultAddr >= Vma->Start && FaultAddr < Vma->End) {
            Found = TRUE;
            break;
        }
        Vma = Vma->Next;
    }

    if (!Found){
        // --- TAMBAHAN DEBUGGING ---
        Serial::Printf("PANIC DEBUG: PF at %p (Task: %d)\n", FaultAddr, Current->pid);
        Serial::Printf("Dumping VMA List for Task %p:\n", Current);
        
        VMArea *DebugVma = Current->VMHead;
        if (!DebugVma) Serial::Printf("  -> VMHead is NULL! (Suspect: Current Task Pointer corrupted?)\n");
        
        while (DebugVma) {
            Serial::Printf("  VMA: [%p - %p] Flags: %x\n", DebugVma->Start, DebugVma->End, DebugVma->Prot);
            DebugVma = DebugVma->Next;
        }
        // --------------------------
        
        return FALSE; // Segfault beneran
    }

    // 2. Cek Permission
    BOOL IsWrite = (ErrorCode & 0x2);
    if (IsWrite && !(Vma->Prot & 0x2)) { 
        Printk::Write(Printk::Level::LOG_ERR, "PF: Write Violation %p\n", FaultAddr);
        return FALSE; 
    }

    // Tentukan Flags
    U64 MapFlags = PAGE_USER | PAGE_PRESENT;
    if (Vma->Prot & 0x2) MapFlags |= PAGE_RW;

    // --- KASUS A: Permission Update (Halaman Sudah Ada) ---
    if (ErrorCode & 0x1) { 
        UPTR OldPhys = PageAlloc::GetPhysicalAddress(UserPML4, PageFaultAddrAligned);
        if (OldPhys) {
            PageAlloc::MapPages(UserPML4, OldPhys, PageFaultAddrAligned, 1, MapFlags);
            Arch::Invlpg(PageFaultAddrAligned); // Jangan lupa flush TLB!
            return TRUE;
        }
    }

    // --- KASUS B: Demand Paging (Halaman Belum Ada) ---
    // Error 0x4 atau 0x6 masuk sini

    UPTR TargetPhysPage = 0;
    BOOL WeAllocatedRAM = FALSE; // Penanda apakah kita pakai RAM baru atau Hardware Address

    // LANGKAH 1: TENTUKAN FISIKNYA DULU (Jangan Alloc dulu!)
    if (Vma->BackingFile) {
        if (Vma->BackingFile->type == FT_SHM) {
            // SHM: Fisik dari Region SHM
            ShmRegion *Region = (ShmRegion*)Vma->BackingFile->PrivateData;
            U64 OffsetInVMA = PageFaultAddrAligned - Vma->Start;
            TargetPhysPage = Region->PhysAddr + Vma->FileOffset + OffsetInVMA;
            // Tidak perlu alloc, tidak perlu baca file (memori sudah shared)
        }
        else if (String::Strcmp((const CHAR8*)Vma->BackingFile->FileName, "/dev/fb0") == 0) {
            // FB: Fisik dari BootInfo
            const BootInfo *Bi = BootInfoGet();
            U64 OffsetInVma = PageFaultAddrAligned - Vma->Start;
            TargetPhysPage = Bi->framebuffer.address + OffsetInVma;
        }
        else {
            // REGULAR FILE (Ex: text.txt, program.elf)
            // Butuh RAM baru
            TargetPhysPage = PageAlloc::PhysicalAllocPages(1);
            if (!TargetPhysPage) return FALSE;
            WeAllocatedRAM = TRUE;
        }
    } else {
        // ANONYMOUS (Stack/Heap)
        // Butuh RAM baru
        TargetPhysPage = PageAlloc::PhysicalAllocPages(1);
        if (!TargetPhysPage) return FALSE;
        WeAllocatedRAM = TRUE;
    }

    // LANGKAH 2: LAKUKAN MAPPING
    // Gunakan TargetPhysPage yang sudah ditentukan di atas
    if (!PageAlloc::MapPages(UserPML4, TargetPhysPage, PageFaultAddrAligned, 1, MapFlags)) {
        if (WeAllocatedRAM) PageAlloc::PhysicalFreePages(TargetPhysPage, 1);
        return FALSE;
    }

    BOOL IsMajor = FALSE;

    // LANGKAH 3: ISI KONTEN (Hanya jika kita alokasi RAM baru)
    if (WeAllocatedRAM) {
        void* KAddr = HHDM_PhysToVirt(TargetPhysPage);
        
        // Apakah ini File Biasa? Baca dari disk!
        if (Vma->BackingFile && Vma->BackingFile->type != FT_SHM /* && not FB */) {
            if (!Vma->BackingFile->Node || !Vma->BackingFile->Node->FSOwner) {
                // Jangan paksa baca kalau FSOwner ga ada! Ini pasti file invalid / dangler!
                Serial::Printf("PF FATAL: BackingFile %s has no FSOwner!\n", Vma->BackingFile->FileName);
                if (WeAllocatedRAM) PageAlloc::PhysicalFreePages(TargetPhysPage, 1);
                return FALSE; 
            }
            
            U64 OffsetInVma = PageFaultAddrAligned - Vma->Start;
            U64 FileOffset = Vma->FileOffset + OffsetInVma;
            
            // Simpan posisi lama
            U64 OldPos = Vma->BackingFile->CurrentPosition;
            Vma->BackingFile->CurrentPosition = FileOffset;
            
            // Baca data
            U64 BytesRead = VFSManager::Read(Vma->BackingFile, (U8*)KAddr, PAGE_SIZE);
            
            // Kembalikan posisi
            Vma->BackingFile->CurrentPosition = OldPos;
            
            // Zeroing sisa halaman jika file habis di tengah halaman
            if (BytesRead < PAGE_SIZE) {
                String::Memset((char*)KAddr + BytesRead, 0, PAGE_SIZE - BytesRead);
            }

            IsMajor = TRUE;
        } 
        else {
            // Anonymous -> Wajib Zeroing (Security)
            String::Memset(KAddr, 0, PAGE_SIZE);
            IsMajor = FALSE;
        }
    } else {
        // Kita pakai fisik yang sudah ada (SHM/FB) -> MINOR FAULT
        IsMajor = FALSE;
    }

    if (IsMajor) {
        Current->CountMajorFault++;
    } else {
        Current->CountMinorFault++;
    }

    return TRUE;
}

// Taruh di mm.cpp atau debugging tools
VOID DebugPageTable(U64 VirtAddr) {
    Tasking::Task *Current = Tasking::GetCurrentTaskPtr();
    U64 *PML4 = HHDM_PhysToVirt(Current->CR3);
    
    SIZE_T PML4_IDX = (VirtAddr >> 39) & 0x1FF;
    SIZE_T PDPT_IDX = (VirtAddr >> 30) & 0x1FF;
    SIZE_T PD_IDX   = (VirtAddr >> 21) & 0x1FF;
    SIZE_T PT_IDX   = (VirtAddr >> 12) & 0x1FF;

    U64 PML4E = PML4[PML4_IDX];
    Serial::Printf("VA %p Dump:\n", VirtAddr);
    Serial::Printf(" PML4[%d] = %llx (RW=%d)\n", PML4_IDX, PML4E, (PML4E & 2)>>1);
    
    if (!(PML4E & 1)) return;
    U64 *PDPT = HHDM_PhysToVirt(PML4E & PAGE_ADDR_MASK);
    U64 PDPTE = PDPT[PDPT_IDX];
    Serial::Printf(" PDPT[%d] = %llx (RW=%d)\n", PDPT_IDX, PDPTE, (PDPTE & 2)>>1);

    if (!(PDPTE & 1)) return;
    U64 *PD = HHDM_PhysToVirt(PDPTE & PAGE_ADDR_MASK);
    U64 PDE = PD[PD_IDX];
    Serial::Printf(" PD  [%d] = %llx (RW=%d)\n", PD_IDX, PDE, (PDE & 2)>>1);

    if (!(PDE & 1)) return;
    U64 *PT = HHDM_PhysToVirt(PDE & PAGE_ADDR_MASK);
    U64 PTE = PT[PT_IDX];
    Serial::Printf(" PT  [%d] = %llx (RW=%d)\n", PT_IDX, PTE, (PTE & 2)>>1);
}

ABI_C void PageFaultHandler(UPTR faulting_address, U64 error_code) {
    // Ambil info bit
    BOOL Present = (error_code & 0x1);
    BOOL Write   = (error_code & 0x2);
    BOOL Usermode = (error_code & 0x4);

    Tasking::Task *Current = Tasking::GetCurrentTaskPtr();

    // KASUS 1: DEMAND PAGING (Halaman tidak ada / Not Present)
    if (Current && Usermode) { 
        if (!Present || (Present && Write)) {
            // Coba perbaiki via VMA manager
            if (HandleUserPageFault(Current, faulting_address, error_code)) {
                return; // Sukses, retry instruction
            }
        }
    }
    
    Arch::ASM::Cli();

    // SAFETY: Matikan FPU/SSE usage di Printk kalau ragu, atau Init FPU
    // Arch::ASM::FPU_Init(); // Uncomment jika VSPrint pakai SSE

    const CHAR8* reason = "Unknown";
    if (Present && Write) reason = "Write Protection Violation (RO Page)";
    else if (!Present) reason = "Page Not Present (Invalid Access)";
    else if (Present && !Write) reason = "Privilege Violation";

    // Debugging Manual tanpa VSPrint kompleks kalau takut crash
    // Printk::Write(LOG_EMERG, "PF at %p err: %x (%s)\n", faulting_address, error_code, reason);

    Serial::Printf(
        "[ISR] Page Fault Exception!\n"
        "       Faulting Address: %p\n"
        "       Error Code: 0x%llx (%s)\n"
        "       Owner PID: %d\n"
        "[ISR] System Halted.\n",
        (void*)faulting_address,
        (unsigned long long)error_code,
        reason,
        (Current ? Current->pid : -1));

    Printk::Panic("Page Fault in area that should not fault.\n");
}

ABI_C VOID GPFaultHandler(U64 error_code, void* reg_context) {
    Arch::ASM::Cli();

    // Decode error code
    BOOL External = (error_code & 0x1);
    U16 Selector = (U16)((error_code >> 3) & 0x1FFF);
    BOOL TI = (error_code & 0x4);  // 0=GDT, 1=LDT
    U16 Index = (U16)((error_code >> 3) & 0x1FFF);

    const CHAR8* TableName = TI ? "LDT" : "GDT";

    // Get register context from stack
    // The assembly pushed registers in this order: r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rbp, rbx, rdx, rcx, rax
    // But we need to access them in saved order: rax, rcx, rdx, rbx, rbp, rsi, rdi, r8, r9, r10, r11, r12, r13, r14, r15
    U64* reg_stack = (U64*)reg_context;
    
    // Calculate offsets based on push order in assembly (15 items, 8 bytes each)
    // Assembly pushed: rax(0), rcx(1), rdx(2), rbx(3), rbp(4), rsi(5), rdi(6), r8(7), r9(8), r10(9), r11(10), r12(11), r13(12), r14(13), r15(14)
    // So to get them back, they're at reg_stack[0..14] in push order
    
    U64 rax = reg_stack[14];  // r15 is at [0], ..., rax at [14] (counting from bottom)
    U64 rcx = reg_stack[13];
    U64 rdx = reg_stack[12];
    U64 rbx = reg_stack[11];
    U64 rbp = reg_stack[10];
    U64 rsi = reg_stack[9];
    U64 rdi = reg_stack[8];
    U64 r8  = reg_stack[7];
    U64 r9  = reg_stack[6];
    U64 r10 = reg_stack[5];
    U64 r11 = reg_stack[4];
    U64 r12 = reg_stack[3];
    U64 r13 = reg_stack[2];
    U64 r14 = reg_stack[1];
    U64 r15 = reg_stack[0];

    Printk::Write(Printk::Level::LOG_ALERT, 
        "\n========== GENERAL PROTECTION FAULT ==========\n");
    
    Printk::Write(Printk::Level::LOG_ALERT,
        "Error Code: 0x%llx (External: %s, Selector: 0x%x, %s, Index: %u)\n",
        (unsigned long long)error_code,
        External ? "YES" : "NO",
        (unsigned)Selector,
        TableName,
        (unsigned)Index);
    
    Printk::Write(Printk::Level::LOG_ALERT,
        "\n--- General Purpose Registers ---\n");
    
    Printk::Write(Printk::Level::LOG_ALERT,
        "RAX: 0x%016llx  RCX: 0x%016llx\n"
        "RDX: 0x%016llx  RBX: 0x%016llx\n"
        "RBP: 0x%016llx  RSI: 0x%016llx\n"
        "RDI: 0x%016llx\n",
        (unsigned long long)rax, (unsigned long long)rcx,
        (unsigned long long)rdx, (unsigned long long)rbx,
        (unsigned long long)rbp, (unsigned long long)rsi,
        (unsigned long long)rdi);

    Printk::Write(Printk::Level::LOG_ALERT,
        "R8:  0x%016llx  R9:  0x%016llx\n"
        "R10: 0x%016llx  R11: 0x%016llx\n"
        "R12: 0x%016llx  R13: 0x%016llx\n"
        "R14: 0x%016llx  R15: 0x%016llx\n",
        (unsigned long long)r8,  (unsigned long long)r9,
        (unsigned long long)r10, (unsigned long long)r11,
        (unsigned long long)r12, (unsigned long long)r13,
        (unsigned long long)r14, (unsigned long long)r15);

    // Get RIP from the ISR context (it's after error code on stack)
    // We need to find RSP to get to the interrupt frame
    // For now, we can try to read from current task
    Tasking::Task* Current = Tasking::GetCurrentTaskPtr();
    if (Current) {
        Printk::Write(Printk::Level::LOG_ALERT,
            "\n--- Task Information ---\n"
            "PID: %llu, PPID: %llu\n",
            (unsigned long long)Current->pid,
            (unsigned long long)Current->ppid);
    }

    Printk::Write(Printk::Level::LOG_EMERG,
        "\n==============================================\n");

    Printk::Panic("General Protection Fault - System Halted\n");
}