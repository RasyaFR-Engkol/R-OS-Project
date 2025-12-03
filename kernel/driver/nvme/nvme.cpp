#include "nvme.hpp"
#include "nvmestructs.hpp"
#include "rossys.hpp"
#include <rosval.h>
#define PRINTK_MODULE_NAME "NVMe"
#include <logging.hpp>
#include <string.hpp>
#include "../../dev/devicemanager.hpp"
#include "../pci/capatibility/msixmsi/msixmsi.hpp"
#include <filesystem/filesystem.hpp>

namespace NVMe{
    using namespace Printk;

    NVMeController* g_NVMeController = nullptr;

    VOID NVMeController::RegisterController(U8 Bus, U8 Device, U8 Function){
        auto *Controller = new NVMeController(Bus, Device, Function);
        g_NVMeController = Controller; // Global pointer set

        // 1. INIT DASAR (Tanpa Interrupt)
        if(!Controller->Initialize()){
            delete Controller;
            Write(Level::LOG_ERR, " Failed to initialize NVMe Controller\n");
            return;
        }

        // 2. IDENTIFY (Polling Mode - Aman dari Crash)
        if(!Controller->IdentifyController()){
            Write(Level::LOG_ERR, " NVMe Identify Failed.\n");
            delete Controller;
            return;
        }
        
        // 3. SETUP INTERRUPT (Baru nyalain MSI-X disini)
        if(!Controller->SetupInterrupts()){
            Write(Level::LOG_WARNING, " NVMe Interrupt Setup Failed (Falling back to polling?)\n");
        }

        // 4. CREATE I/O QUEUES (Pake Interrupt yg udah nyala)
        if (!Controller->CreateIOQueues()) {
             Write(Level::LOG_ERR, " Failed to create I/O Queues.\n");
             delete Controller;
             return;
        }

        Write(Level::LOG_INFO, " NVMe Initialization Complete. Registered as 'nvme0n1'\n");
        DeviceManager::RegisterBlockDevice(Controller);
    }

    NVMeController::NVMeController(U8 bus, U8 dev, U8 func) 
        : Bus(bus), Device(dev), Function(func), AdminSQTail(0), AdminCQHead(0), PhaseTag(1) {
            CHAR8 NameNVMeBuffer[16];
            String::SPrint((char*)NameNVMeBuffer, sizeof(NameNVMeBuffer), "nvme%dn1", 0);
            FileSystem* fs = nullptr; char rel[256];
            if (VFSManager::ResolvePath((const char*)"/dev", &fs, rel) && fs) {
                DevFS* devfs = (DevFS*)fs;
                devfs->RegisterBlockDevice(this, NameNVMeBuffer);
            }
        }

    // Logic Interrupt
    void NVMeController::HandleInterrupt() {
        // 1. Cek Admin Queue
        ProcessCompletionQueue(this->AdminCQBase, this->AdminCQHead, this->PhaseTag, 0);

        // 2. Cek I/O Queue (QID 1)
        if (this->iOCQBase != nullptr) {
            ProcessCompletionQueue(this->iOCQBase, this->iOCQHead, this->iOPhaseTag, 1);
        }
    }

    struct CQResult {
        U16 Processed;
        U16 Status;
    };

    void NVMeController::InterruptHandler(void* Context) {
        // HIRAUKAN CONTEXT DARI ARGUMEN KARENA SERINGKALI INVALID/SAMPAH
        // Gunakan Global Pointer yang sudah pasti valid
        if (g_NVMeController) {
            g_NVMeController->HandleInterrupt();
        }
    }

    U16 NVMeController::ProcessCompletionQueue(NVMeCompletion* CQBase, U16& Head, U8& PTag, U16 QueueID) {
        U16 ProcessedCount = 0;
        
        while (TRUE) {
            volatile NVMeCompletion* Cpl = &CQBase[Head];
            U16 StatusVal = Cpl->Status;
            
            // Ambil Phase Tag (Bit 0 dari Status U16, atau Bit 16 dari Dword 3)
            U8 P = (StatusVal >> 0) & 1; // Asumsi bit struct lo bener
            
            // Kalau Phase Tag di memori != Phase Tag harapan driver, 
            // berarti Controller BELUM nulis disitu. Stop.
            if (P != PTag) {
                break;
            }

            // === ADA COMMAND SELESAI ===
            ProcessedCount++;
            
            // Cek Error
            U16 StatusCode = (StatusVal >> 1) & 0x7FFF; // Bit 1-15
            U16 CID = Cpl->CommandID;
            
            if (StatusCode != 0) {
                Write(Level::LOG_ERR, " [NVMe] Cmd ID %d Failed! Status: 0x%x\n", CID, StatusCode);
            } else {
                // Command Sukses!
                // Nanti disini lo mapping CID ke Request asli buat notify OS.
                // Misal: RequestTable[CID].Complete = TRUE;
            }

            // Majuin Head
            Head = (Head + 1) % 32; // Ingat AQA kita 32 entry

            // Kalau wrap-around, flip Phase Tag harapan driver
            if (Head == 0) {
                PTag = !PTag;
            }
        }

        // Kalau ada yang diproses, update Doorbell biar Controller tau kita udah baca
        if (ProcessedCount > 0) {
            WriteDoorbell(QueueID, Head, TRUE); // TRUE = Completion Queue Doorbell
        }

        return ProcessedCount;
    }

    BOOL NVMeController::Initialize(){
        U16 CMD = PCI::ReadWord(Bus, Device, Function, 0x04);
        CMD |= (1 << 2) | (1 << 1) | (1 << 10);
        PCI::WriteWord(Bus, Device, Function, 0x04, CMD);

        U32 Bar0 = PCI::ReadDword(Bus, Device, Function, 0x10);
        U32 Bar1 = PCI::ReadDword(Bus, Device, Function, 0x14);
        U64 BarPhys = (Bar0 & 0xFFFFFFF0);
        if((Bar0 & 0x06) == 0x4) BarPhys |= ((U64)Bar1 << 32);

        U64 PageCount = 4;
        VOID *VirtAddr = PageAlloc::VirtualAllocPages(PageCount);
        if(!PageAlloc::MapPages(KernelPML4, BarPhys, (U64)VirtAddr, PageCount, PAGE_PRESENT | PAGE_RW | PAGE_PCD)){
            return FALSE;
        }
        this->Regs = (NVMeRegister*)VirtAddr;
        this->Stride = (Regs->CAP >> 32) & 0xF;

        // Reset Controller
        if(Regs->CSTS & 0x01){
            Regs->CC &= ~0x01;
            while(Regs->CSTS & 0x01) Arch::ASM::CPURelax();
        }

        // -- CATATAN: KODE MSI DISINI DIHAPUS, DIPINDAH KE SetupInterrupts --

        this->AdminSQBuf = PageAlloc::DMAAlloc::AllocateDMAPages(1);
        this->AdminCQBuf = PageAlloc::DMAAlloc::AllocateDMAPages(1);
        this->AdminSQBase = (NVMeCommand*)this->AdminSQBuf->VirtAddr;
        this->AdminSQPhys = this->AdminSQBuf->PhysAddr;
        this->AdminCQBase = (NVMeCompletion*)this->AdminCQBuf->VirtAddr;
        this->AdminCQPhys = this->AdminCQBuf->PhysAddr;

        String::Memset(this->AdminSQBase, 0, 4096);
        String::Memset(this->AdminCQBase, 0, 4096);

        Regs->AQA = (0x001F << 16) | 0x001F;
        Regs->ASQ = AdminSQPhys;
        Regs->ACQ = AdminCQPhys;

        U32 cc = 1; // EN=1
        cc |= (6 << 16); cc |= (4 << 20);
        Regs->CC = cc;

        while ((Regs->CSTS & 0x1) == 0) Arch::ASM::CPURelax();
        
        return TRUE;
    }

    BOOL NVMeController::SetupInterrupts() {
        U8 MSIXCap = PCI::FindCapability(Bus, Device, Function, 0x11);
        if (MSIXCap) {
             Write(Level::LOG_INFO, " [NVMe] Enabling MSI-X...\n");
             this->InterruptVector = MSI::EnableMSIX(Bus, Device, Function, MSIXCap, &NVMeController::InterruptHandler); 
             if (this->InterruptVector > 0) return TRUE;
        }
        return FALSE;
    }

    VOID NVMeController::WriteDoorbell(U16 QueueID, U16 Val, BOOL IsCQ) {
        // Rumus offset: 0x1000 + (2 * QueueID + (IsCQ ? 1 : 0)) * (4 << Stride)
        // Kita pakai pointer arithmetic via base address Regs
        
        UPTR DoorbellBase = (UPTR)Regs + 0x1000;
        UPTR Index = (2 * QueueID) + (IsCQ ? 1 : 0);
        UPTR Offset = Index * (4 << Stride);
        
        volatile U32* DBRegister = (volatile U32*)(DoorbellBase + Offset);
        *DBRegister = Val;
    }

    BOOL NVMeController::IdentifyController(){
        auto *IDBuffer = PageAlloc::DMAAlloc::AllocateDMAPages(1);
        String::Memset((VOID*)IDBuffer->VirtAddr, 0, 4096);
        NVMeCommand cmd = {};
        cmd.Opcode = 0x06; cmd.CommandID = 1; cmd.NSID = 0;
        cmd.DataPtr1 = IDBuffer->PhysAddr; cmd.Dword10 = 1;
        
        this->AdminSQBase[this->AdminSQTail] = cmd;
        this->AdminSQTail = (this->AdminSQTail + 1) % 32;
        WriteDoorbell(0, this->AdminSQTail, FALSE);

        if(!PollAdminCompletion(1)){
            PageAlloc::DMAAlloc::FreeDMABuffer(IDBuffer);
            return FALSE;
        }
        
        auto *Data = (NVMeIdentifyControllerStruct*)IDBuffer->VirtAddr;
        Write(Level::LOG_INFO, " [NVMe] Model: %.40s\n", Data->MN);
        PageAlloc::DMAAlloc::FreeDMABuffer(IDBuffer);

        return IdentifyNamespace();
    }

    BOOL NVMeController::IdentifyNamespace(){
        auto *IDBuffer = PageAlloc::DMAAlloc::AllocateDMAPages(1);
        String::Memset((VOID*)IDBuffer->VirtAddr, 0, 4096);

        NVMeCommand cmd = {};
        cmd.Opcode = 0x06; // Identify
        cmd.CommandID = 2; // ID beda dikit biar gampang debug
        cmd.NSID = 1;      // Namespace ID 1 (Biasanya namespace utama)
        cmd.DataPtr1 = IDBuffer->PhysAddr;
        cmd.Dword10 = 0;   // CNS = 0 (Identify Namespace)

        U16 Tail = this->AdminSQTail;
        this->AdminSQBase[Tail] = cmd;
        this->AdminSQTail = (Tail + 1) % 32;
        WriteDoorbell(0, this->AdminSQTail, FALSE);

        if(!PollAdminCompletion(2)){
            Write(Level::LOG_ERR, " [NVMe] Identify Namespace Failed!\n");
            PageAlloc::DMAAlloc::FreeDMABuffer(IDBuffer);
            return FALSE;
        }

        auto *Data = (NVMeIdentifyNamespaceStruct*)IDBuffer->VirtAddr;
        
        this->LBACount = Data->NSZE; // Total Sector
        
        // Cek format LBA (LBA Format 0 biasanya default)
        // FLBAS (Formatted LBA Size) bits 0-3 index ke tabel LBAF
        // Untuk simpel, kita asumsikan LBAF[0] aktif atau cek bit 4 buat metadata.
        // Tapi cara paling gampang buat logic LBA size (shift factor):
        U8 LBAF_Index = Data->FLBAS & 0xF;
        UNUSED__ U8 LBA_DS = (Data->NLBAF >> (LBAF_Index * 0)) & 0xFF; // Logic ini kompleks ambil dari struct array LBAF
        // SHORTCUT: Identify Namespace Struct byte 128 (LBA Format 0 Support)
        // Byte 128 bit 16-23 is LBA Data Size (power of 2).
        // Biasanya 9 (2^9 = 512) atau 12 (2^12 = 4096).
        
        // Kita pakai hardcode logic sederhana dulu:
        // Cek LBA Format 0 (Data->LBAF[0]) -> Ini ada di array struct sebenernya
        // Untuk sekarang kita asumsikan 512 bytes kalau pusing parsing arraynya, 
        // ATAU implementasi proper struct LBAF.
        
        // Fallback safe: 512 bytes
        this->LBASize = 512; 
        
        Write(Level::LOG_INFO, " [NVMe] Namespace 1 Active. Size: %lld sectors (%lld MB)\n", 
              this->LBACount, (this->LBACount * this->LBASize) / (1024*1024));

        PageAlloc::DMAAlloc::FreeDMABuffer(IDBuffer);
        return TRUE;
    }

    BOOL NVMeController::PollAdminCompletion(U16 CID){
        U16 Head = this->AdminCQHead;
        volatile NVMeCompletion* Cpl = &this->AdminCQBase[Head];
        int Timeout = 1000000;
        
        while (TRUE) {
            U16 StatusVal = Cpl->Status;
            
            // FIX DISINI: Phase Tag adalah Bit LSB (Bit 0)
            U8 P = StatusVal & 0x01; 

            if (P == this->PhaseTag) {
                break;
            }

            if (Timeout-- <= 0) {
                Write(Level::LOG_ERR, " NVMe: Command Timeout waiting for CID %d\n", CID);
                return FALSE;
            }
            Arch::ASM::CPURelax();
        }

        // Cek Status Error (Shift 1 bit ke kanan untuk buang Phase Tag)
        // Mask 0x7FFF untuk ambil sisa 15 bit Status Code
        U16 StatusCode = (Cpl->Status >> 1) & 0x7FFF;
        
        if (StatusCode != 0) {
            Write(Level::LOG_ERR, " NVMe: Command Error. Status: 0x%x (SC: 0x%x)\n", Cpl->Status, StatusCode);
            // return FALSE; // Uncomment kalau mau strict
        }

        this->AdminCQHead = (Head + 1) % 32;
        
        if (this->AdminCQHead == 0) {
            this->PhaseTag = !this->PhaseTag;
        }

        WriteDoorbell(0, this->AdminCQHead, TRUE);
        return TRUE;
    }
    // IBlockDevice interface: minimal stubs for now
    BOOL NVMeController::ReadSectors(U64 LBA, U32 Count, PageAlloc::DMAAlloc::DMABuffer **BufferOut){
        // 1. Alokasi Buffer DMA buat nampung data dari disk
        // Hitung butuh berapa page. Asumsi 1 sector = 512 bytes.
        U64 TotalBytes = Count * this->LBASize;
        U64 PagesNeeded = (TotalBytes + 4095) / 4096;
        
        auto *Buf = PageAlloc::DMAAlloc::AllocateDMAPages(PagesNeeded);
        if (!Buf) return FALSE;
        
        String::Memset((void*)Buf->VirtAddr, 0, TotalBytes);

        // 2. Siapkan Command NVMe (NVM Command Set)
        NVMeCommand cmd = {};
        cmd.Opcode = 0x02; // 0x02 = Read, 0x01 = Write
        cmd.CommandID = 0x55; // ID bebas, asal unik per request
        cmd.NSID = 1;      // Namespace ID (biasanya 1)
        
        // Alamat Buffer Fisik (PRP1)
        // NOTE: Kalau buffer > 4KB atau nyebrang page boundary, lo butuh PRP List (PRP2).
        // Untuk simpel, kita asumsi buffer contiguous fisik dan < 4KB atau aligned sempurna dulu.
        cmd.DataPtr1 = Buf->PhysAddr; 
        
        // SLBA (Starting LBA) - 64 bit split ke 2 Dword
        cmd.Dword10 = (U32)LBA;
        cmd.Dword11 = (U32)(LBA >> 32);
        
        // NLB (Number of Logical Blocks) - 0 based (Count - 1)
        // Bit 0-15: NLB. 
        cmd.Dword12 = (Count - 1) & 0xFFFF;

        // 3. Masukkan ke Submission Queue I/O (QID 1)
        // Hati-hati race condition kalau multithread, perlu Spinlock disini.
        
        U16 Tail = this->iOSQTail;
        this->iOSQBase[Tail] = cmd;
        
        // Update Tail Driver local
        this->iOSQTail = (Tail + 1) % 32; // Asumsi queue depth 32
        
        // 4. Ring Doorbell I/O (QID 1, bukan 0!)
        WriteDoorbell(1, this->iOSQTail, FALSE); // FALSE = Submission

        // 5. Tunggu Completion
        // Karena lo udah setup MSI-X, idealnya lo 'Sleep' thread ini dan dibangunin sama Interrupt Handler.
        // TAPI untuk tes awal: Polling dulu queue I/O-nya manual kayak admin tadi.
        
        // ... logic polling completion QID 1 ...
        // (Bisa copas logic PollAdminCompletion tapi ganti target ke iOCQBase & iOCQHead)
        
        // SEMENTARA: Polling bodoh buat tes
        int Timeout = 1000000;
        while(Timeout--) {
            volatile NVMeCompletion* Cpl = &this->iOCQBase[this->iOCQHead];
            // Inget fix Phase Tag tadi!
            if((Cpl->Status & 1) == this->iOPhaseTag) {
                // Found completion!
                
                // Update Head & PhaseTag
                this->iOCQHead = (this->iOCQHead + 1) % 32;
                if(this->iOCQHead == 0) this->iOPhaseTag = !this->iOPhaseTag;
                
                // Ring Doorbell Completion
                WriteDoorbell(1, this->iOCQHead, TRUE);
                
                *BufferOut = Buf;
                return TRUE;
            }
            Arch::ASM::CPURelax();
        }
        
        return FALSE;
    }

    BOOL NVMeController::WriteSectors(U64 LBA, U32 Count, PageAlloc::DMAAlloc::DMABuffer *Buffer){
        if(!Buffer || Count == 0) return FALSE;

        NVMeCommand cmd = {};
        cmd.Opcode = 0x01; // Write
        cmd.CommandID = 0x56;
        cmd.NSID = 1;
        cmd.DataPtr1 = Buffer->PhysAddr;
        cmd.Dword10 = (U32)LBA;
        cmd.Dword11 = (U32)(LBA >> 32);
        cmd.Dword12 = (Count - 1) & 0xFFFF;

        U16 Tail = this->iOSQTail;
        this->iOSQBase[Tail] = cmd;
        this->iOSQTail = (Tail + 1) % 32;

        WriteDoorbell(1, this->iOSQTail, FALSE);

        int Timeout = 1000000;
        while(Timeout--) {
            volatile NVMeCompletion* Cpl = &this->iOCQBase[this->iOCQHead];
            
            if((Cpl->Status & 1) == this->iOPhaseTag) {
                // Check status code (Shift right 1 bit karena bit 0 itu Phase Tag)
                // Bit 17-31: Status Code Type & Value. 0 = Success.
                if ((Cpl->Status >> 1) != 0) {
                    // Error handling: Write Failed
                    // Balikin Head biar ga macet, tapi return FALSE
                    this->iOCQHead = (this->iOCQHead + 1) % 32;
                    if(this->iOCQHead == 0) this->iOPhaseTag = !this->iOPhaseTag;
                    WriteDoorbell(1, this->iOCQHead, TRUE);
                    return FALSE;
                }

                // Success
                this->iOCQHead = (this->iOCQHead + 1) % 32;
                if(this->iOCQHead == 0) this->iOPhaseTag = !this->iOPhaseTag;
                
                WriteDoorbell(1, this->iOCQHead, TRUE); // Completion Doorbell QID 1
                return TRUE;
            }
            Arch::ASM::CPURelax();
        }

        return FALSE;
    }

    BOOL NVMeController::FlushCache(){
        NVMeCommand cmd = {};
        cmd.Opcode = 0x00;
        cmd.CommandID = 0x99;
        cmd.NSID = 1;

        U16 Tail = this->iOSQTail;
        this->iOSQBase[Tail] = cmd;

        this->iOSQTail = (Tail + 1) % 32;

        WriteDoorbell(1, this->iOSQTail, FALSE);

        int Timeout = 1000000; // Flush mungkin butuh waktu agak lama
        while(Timeout--) {
            volatile NVMeCompletion* Cpl = &this->iOCQBase[this->iOCQHead];
            
            if((Cpl->Status & 1) == this->iOPhaseTag) {
                
                // Cek Error
                if ((Cpl->Status >> 1) != 0) {
                    // Flush gagal
                    this->iOCQHead = (this->iOCQHead + 1) % 32;
                    if(this->iOCQHead == 0) this->iOPhaseTag = !this->iOPhaseTag;
                    WriteDoorbell(1, this->iOCQHead, TRUE);
                    return FALSE;
                }

                // Success
                this->iOCQHead = (this->iOCQHead + 1) % 32;
                if(this->iOCQHead == 0) this->iOPhaseTag = !this->iOPhaseTag;
                
                WriteDoorbell(1, this->iOCQHead, TRUE);
                return TRUE;
            }
            Arch::ASM::CPURelax();
        }

        return FALSE;
    }

    BOOL NVMeController::CreateIOQueues() {
        // Alokasi
        this->iOCQBuf = PageAlloc::DMAAlloc::AllocateDMAPages(1);
        String::Memset((void*)this->iOCQBuf->VirtAddr, 0, 4096);
        this->iOCQBase = (NVMeCompletion*)this->iOCQBuf->VirtAddr;
        this->iOCQPhys = this->iOCQBuf->PhysAddr;
        this->iOCQHead = 0;
        this->iOPhaseTag = 1;

        this->iOSQBuf = PageAlloc::DMAAlloc::AllocateDMAPages(1);
        String::Memset((void*)this->iOSQBuf->VirtAddr, 0, 4096);
        this->iOSQBase = (NVMeCommand*)this->iOSQBuf->VirtAddr;
        this->iOSQPhys = this->iOSQBuf->PhysAddr;
        this->iOSQTail = 0;

        // 1. Create Completion Queue
        NVMeCommand cmd = {};
        cmd.Opcode = 0x05;
        cmd.CommandID = 0x10;
        cmd.Dword10 = (31 << 16) | 1; // Size=32, QID=1
        // FIX: MSI-X Index = 0 (bukan Vector ID 0x85)
        cmd.Dword11 = (0 << 16) | 0x03; 
        cmd.DataPtr1 = this->iOCQPhys;

        U16 Tail = this->AdminSQTail;
        this->AdminSQBase[Tail] = cmd;
        this->AdminSQTail = (Tail + 1) % 32;
        WriteDoorbell(0, this->AdminSQTail, FALSE);

        if (!PollAdminCompletion(0x10)) {
            Write(Level::LOG_ERR, " [NVMe] Create CQ Failed\n");
            return FALSE;
        }

        // 2. Create Submission Queue
        String::Memset(&cmd, 0, sizeof(NVMeCommand));
        cmd.Opcode = 0x01;
        cmd.CommandID = 0x11;
        cmd.Dword10 = (31 << 16) | 1; 
        cmd.Dword11 = (1 << 16) | 0x01; 
        cmd.DataPtr1 = this->iOSQPhys;

        Tail = this->AdminSQTail;
        this->AdminSQBase[Tail] = cmd;
        this->AdminSQTail = (Tail + 1) % 32;
        WriteDoorbell(0, this->AdminSQTail, FALSE);

        if (!PollAdminCompletion(0x11)) {
            Write(Level::LOG_ERR, " [NVMe] Create SQ Failed\n");
            return FALSE;
        }

        Write(Level::LOG_INFO, " [NVMe] I/O Queues Created (QID 1)\n");
        return TRUE;
    }

}