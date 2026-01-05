#include "virtio_gpu.hpp"
#include "rossys.hpp"
#include "rosval.h"
#include "virtio_structs.hpp"
#include <../kernel/driver/pci/pci.hpp>
#include <../kernel/driver/pci/capatibility/msixmsi/msixmsi.hpp>
#define PRINTK_MODULE_NAME "QemuVirtio"
#include <logging.hpp>
#include <string.hpp>
#include <framebuffer.hpp>
#include <../kernel/log/fbcon/fbcon.hpp>
#include <bootinfo.h>

namespace VirtioGPU{
    Driver GlobalDriver;

    static UPTR MapBar(U8 Bus, U8 Dev, U8 Func, U8 BarIndex){
        U32 BarLo = PCI::ReadDword(Bus, Dev, Func, 0x10 + (BarIndex * 4));
        U64 BarAddr = BarLo & 0xFFFFFFF0; // Masking tipe bit (IO/Mem)

        if((BarLo & 0x06) == 0x04){ // 64-bit BAR
            U32 BarHi = PCI::ReadDword(Bus, Dev, Func, 0x10 + (BarIndex * 4) + 4);
            BarAddr |= ((U64)BarHi << 32);
        }

        // Hati-hati: BarAddr bisa 0 jika device belum di-init BIOS/OS
        if (BarAddr == 0) return 0;

        // --- MMIO MAPPING ---
        // Kita alokasikan 2 halaman (8KB) cukup untuk config struct Virtio.
        // Kalau mau proper, harus baca ukuran BAR dulu, tapi 2 page biasanya cukup buat header.
        SIZE_T numPages = 8; 
        
        // 1. Alokasi Virtual Address Baru
        void* virtAddr = PageAlloc::VirtualAllocPages(numPages);
        if (!virtAddr) {
            Printk::Write(Printk::Level::LOG_EMERG, "VirtioGPU: Failed to alloc virtual pages for BAR\n");
            return 0;
        }

        // 2. Petakan Fisik (BAR) ke Virtual
        // PENTING: Gunakan PAGE_PCD (Cache Disable) karena ini IO Device!
        if (!PageAlloc::MapPages(KernelPML4, BarAddr, (UPTR)virtAddr, numPages, PAGE_PRESENT | PAGE_RW | PAGE_PCD)) {
             Printk::Write(Printk::Level::LOG_EMERG, "VirtioGPU: Failed to map BAR\n");
             return 0;
        }

        return (UPTR)virtAddr;
    }

    VOID Driver::ParseCapabilities(){
        // CapPtr pertama ada di offset 0x34
        U8 CapPtr = PCI::ReadByte(Bus, Dev, Function, 0x34);

        // Safety limit biar ga infinite loop kalau hardware rusak/malicious
        int loopSafety = 0; 

        while(CapPtr != 0 && loopSafety < 48){ // 48 is enough standard max cap space
            // Printk::Write(Printk::Level::LOG_DEBUG, "Loop CapPtr: 0x%x\n", CapPtr);
            
            U8 capId = PCI::ReadByte(Bus, Dev, Function, CapPtr);
            U8 capNext = PCI::ReadByte(Bus, Dev, Function, CapPtr + 1); // Ini alamat absolut next cap

            if(capId == 0x09){ // Vendor Specific for Virtio
                U8 CFGType = PCI::ReadByte(Bus, Dev, Function, CapPtr + 3);
                U8 BarIndex = PCI::ReadByte(Bus, Dev, Function, CapPtr + 4);
                
                // [FIX 2] Offset itu 32-bit (4 bytes), bukan 1 byte!
                U32 Offset = PCI::ReadDword(Bus, Dev, Function, CapPtr + 8); 
                __MAYBE_UNUSED U32 Length = PCI::ReadDword(Bus, Dev, Function, CapPtr + 12); 

                UPTR BarBase = MapBar(Bus, Dev, Function, BarIndex);

                if (CFGType == VIRTIO_PCI_CAP_COMMON_CFG) {
                    CommonCfg = (VirtioPciCommonCfg*)(BarBase + Offset);
                    //Printk::Write(Printk::Level::LOG_INFO, "VirtioGPU: Common Cfg found at %p\n", CommonCfg);
                }
                else if (CFGType == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                    NotifyBase = (U8*)(BarBase + Offset);
                    NotifyMultiplier = PCI::ReadDword(Bus, Dev, Function, CapPtr + 16);
                    //Printk::Write(Printk::Level::LOG_INFO, "VirtioGPU: Notify Cfg found\n");
                }
                else if (CFGType == VIRTIO_PCI_CAP_DEVICE_CFG) {
                    DeviceCfg = (U8*)(BarBase + Offset);
                }
            }

            // [FIX 1] Update pointer ke next capability (Absolute, bukan ditambah)
            CapPtr = capNext;
            
            loopSafety++;
        }
    }

    VOID Driver::SetupQueue(int QueueIndex){
        CommonCfg->queue_select = QueueIndex;
        U16 Size = CommonCfg->queue_size;

        if(Size == 0) return;

        VirtQueue *VQ = &Queues[QueueIndex];
        VQ->QueueSize = Size;
        VQ->AvailableIdx = 0;
        VQ->LastUsedIdx = 0;
        VQ->NotifyOffset = CommonCfg->queue_notify_off;

        VQ->DescBuf = PageAlloc::DMAAlloc::AllocateDMABytes(Size * 16);
        VQ->Desc = (VRingDesc*)VQ->DescBuf->VirtAddr;

        VQ->AvailBuf = PageAlloc::DMAAlloc::AllocateDMABytes(6 + 2 * Size);
        VQ->Avail = (VRingAvail*)VQ->AvailBuf->VirtAddr;

        VQ->UsedBuf = PageAlloc::DMAAlloc::AllocateDMABytes(6 + 8 * Size);
        VQ->Used = (VRingUsed*)VQ->UsedBuf->VirtAddr;

        CommonCfg->queue_desc_lo = (U32)VQ->DescBuf->PhysAddr;
        CommonCfg->queue_desc_hi = (U32)(VQ->DescBuf->PhysAddr >> 32);
        
        CommonCfg->queue_avail_lo = (U32)VQ->AvailBuf->PhysAddr;
        CommonCfg->queue_avail_hi = (U32)(VQ->AvailBuf->PhysAddr >> 32);
        
        CommonCfg->queue_used_lo = (U32)VQ->UsedBuf->PhysAddr;
        CommonCfg->queue_used_hi = (U32)(VQ->UsedBuf->PhysAddr >> 32);

        CommonCfg->queue_enable = 1;
        
        //Printk::Write(Printk::Level::LOG_INFO, "VirtioGPU: Queue %d setup done (Size: %d)\n", QueueIndex, Size);
    }

    VOID Driver::NotifyQueue(int qIdx){
        UPTR notifyAddr = (UPTR)NotifyBase + (Queues[qIdx].NotifyOffset * NotifyMultiplier);
        *(volatile U16*)notifyAddr = (U16)qIdx;
    }

    BOOL Driver::SendCommand(U64 PhysCmd, U32 CMDSize, U64 PhysResp, U32 RespSize){
        VirtQueue *VQ = &Queues[0];

        U16 Idx = VQ->AvailableIdx % VQ->QueueSize;
        
        // --- DESCRIPTOR 1: COMMAND (READ-ONLY) ---
        // Langsung pakai alamat fisik yang dikirim caller
        VQ->Desc[Idx].addr = PhysCmd; 
        VQ->Desc[Idx].len = CMDSize;
        VQ->Desc[Idx].flags = 1; // NEXT
        VQ->Desc[Idx].next = (Idx + 1) % VQ->QueueSize;

        // --- DESCRIPTOR 2: RESPONSE (WRITE-ONLY) ---
        U16 NextIdx = (Idx + 1) % VQ->QueueSize;
        VQ->Desc[NextIdx].addr = PhysResp;
        VQ->Desc[NextIdx].len = RespSize;
        VQ->Desc[NextIdx].flags = 2; // WRITE
        VQ->Desc[NextIdx].next = 0;

        // Update Available Ring
        VQ->Avail->ring[VQ->Avail->idx % VQ->QueueSize] = Idx;

        Arch::ASM::Clflush(&VQ->Desc[Idx]);
        Arch::ASM::Clflush(&VQ->Desc[(Idx + 1) % VQ->QueueSize]);
    
        // Flush Available Ring tipis-tipis
        Arch::ASM::Clflush(&VQ->Avail->ring[VQ->Avail->idx % VQ->QueueSize]);

        Arch::ASM::Mfence(); // Memory Barrier PENTING

        VQ->Avail->idx++;

        Arch::ASM::Mfence();

        VQ->AvailableIdx += 2;

        NotifyQueue(0);

        // Tambahin Timeout agak panjang dikit buat init (misal 100000 atau pake timer OS)
        // QEMU kadang butuh waktu alokasi resource di host.

        INTN Timeout = 1000000; // Gedein timeoutnya
        while(VQ->LastUsedIdx == VQ->Used->idx && Timeout--){
            Arch::ASM::CPURelax();
            
            Arch::ASM::Clflush(&VQ->Used->idx);
        }

        if (Timeout <= 0) {
             Printk::Write(Printk::Level::LOG_DERR, "Timeout! Stuck at LastUsed: %d == DeviceUsed: %d\n", VQ->LastUsedIdx, VQ->Used->idx);
             return FALSE;
        }
        VQ->LastUsedIdx++;

        return TRUE;
    }

    VOID Driver::Initialize(U8 bus, U8 dev, U8 func){
        Bus = bus, Dev = dev, Function = func;

        // 1. Enable Bus Master
        U16 CMD = PCI::ReadDword(Bus, Dev, Function, 0x04);
        PCI::WriteWord(Bus, Dev, Function, 0x04, CMD | 0x07); 

        ParseCapabilities();
        if(!CommonCfg){
            Printk::Write(Printk::Level::LOG_ERR, "VirtioGPU: Failed to find Common Cfg!\n");
            return;
        }

        // 2. Reset Device
        CommonCfg->device_status = 0;
        // Wait sedikit biar reset tuntas
        for(int i=0; i<1000; i++);

        // 3. Set Acknowledge & Driver
        CommonCfg->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;

        // 4. FEATURE NEGOTIATION (CRITICAL FIX)
        // Kita harus set bit 32 (VIRTIO_F_VERSION_1) biar QEMU tau kita pake Modern Interface.
        // Fitur ada di 2 Bank (32-bit masing-masing).
        
        // Bank 0 (Bit 0-31): Kita tolak semua fitur aneh (0)
        CommonCfg->driver_feature_select = 0;
        CommonCfg->driver_feature = 0; 

        // Bank 1 (Bit 32-63): Kita aktifkan VIRTIO_F_VERSION_1 (Bit 32 -> Bit 0 di Bank 1)
        CommonCfg->driver_feature_select = 1;
        CommonCfg->driver_feature = 1; // 1 berarti (1 << (32-32)) alias Bit 32 nyala.

        // 5. Finalize Features
        CommonCfg->device_status |= VIRTIO_STATUS_FEATURES_OK;

        // 6. CHECK FEATURES_OK (Wajib di Spec Virtio 1.0)
        if (!(CommonCfg->device_status & VIRTIO_STATUS_FEATURES_OK)) {
            Printk::Write(Printk::Level::LOG_EMERG, "VirtioGPU: Feature Negotiation Failed! QEMU rejects us.\n");
            return;
        }

        //Printk::Write(Printk::Level::LOG_DEBUG, "Virtio Features Negotiated.\n");

        // 7. Setup Queues
        SetupQueue(0);
        SetupQueue(1);

        // 8. Driver OK (Sekarang device boleh baca queue)
        CommonCfg->device_status |= VIRTIO_STATUS_DRIVER_OK;

        FlushEvents();

        const BootInfo *BI = BootInfoGet();
        
        U64 WidthSc = BI->framebuffer.width;
        U64 HeightSc = BI->framebuffer.height;
        if(!WidthSc || !HeightSc || !BI){
            Printk::Write(Printk::Level::LOG_WARNING, "VirtioGPU: No Framebuffer default resolution found. Fixed to 1280x720\n");
            WidthSc = 1080;
            HeightSc = 720;
        }

        SetupScanout(WidthSc, HeightSc);
    }

    void Driver::HardwareFlush(U32 x, U32 y, U32 w, U32 h) {
        Driver* drv = &GlobalDriver;
        if (!drv->CmdBuffer) return;

        U8* cmdPtr = (U8*)drv->CmdBuffer->VirtAddr;
        U64 physBase = drv->CmdBuffer->PhysAddr;
        U32 screenW = drv->CurrentWidth; // <--- Ambil dinamis

        for(U64 i=0; i < sizeof(VirtioGpuTransferToHost2D); i += 64) {
            Arch::ASM::Clflush(cmdPtr + i);
        }

        // --- 1. TRANSFER TO HOST ---
        VirtioGpuTransferToHost2D* cmdTrans = (VirtioGpuTransferToHost2D*)cmdPtr;
        VirtioGpuCtrlHeader* resp = (VirtioGpuCtrlHeader*)(cmdPtr + sizeof(VirtioGpuTransferToHost2D));

        cmdTrans->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
        cmdTrans->resource_id = 1;
        cmdTrans->r.x = x;
        cmdTrans->r.y = y;
        cmdTrans->r.width = w;
        cmdTrans->r.height = h;
        
        // [FIX] Gunakan variabel screenW, bukan 1920
        cmdTrans->offset = (y * screenW + x) * 4; 

        drv->SendCommand(physBase, sizeof(*cmdTrans), physBase + sizeof(*cmdTrans), sizeof(*resp));

        // --- 2. RESOURCE FLUSH ---
        VirtioGpuResourceFlush* cmdFlush = (VirtioGpuResourceFlush*)cmdPtr;
        
        String::Memset(cmdPtr, 0, 1024);
        
        cmdFlush->hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
        cmdFlush->resource_id = 1;
        cmdFlush->r.x = x;
        cmdFlush->r.y = y;
        cmdFlush->r.width = w;
        cmdFlush->r.height = h;

        drv->SendCommand(physBase, sizeof(*cmdFlush), physBase + sizeof(*cmdFlush), sizeof(*resp));
    }

    VOID Driver::FlushEvents() {
        // Kita pake buffer CMD yang ada aja
        if (this->CmdBuffer) PageAlloc::DMAAlloc::FreeDMABuffer(this->CmdBuffer);
        this->CmdBuffer = PageAlloc::DMAAlloc::AllocateDMABytes(4096);
        
        U8* cmdPtr = (U8*)this->CmdBuffer->VirtAddr;
        U64 physAddr = this->CmdBuffer->PhysAddr;

        VirtioGpuCtrlHeader* cmd = (VirtioGpuCtrlHeader*)cmdPtr;
        __MAYBE_UNUSED VirtioGpuCtrlHeader* resp = (VirtioGpuCtrlHeader*)(cmdPtr + sizeof(VirtioGpuCtrlHeader));
        
        // Command 0x0100 = VIRTIO_GPU_CMD_GET_DISPLAY_INFO
        // Cuma butuh header doang, ga ada body.
        String::Memset(cmdPtr, 0, 4096);
        cmd->type = 0x0100; 

        // FLUSH COMMAND KE RAM (Wajib, kayak kemarin)
        for(U32 i=0; i<sizeof(VirtioGpuCtrlHeader); i+=64) Arch::ASM::Clflush(cmdPtr + i);
        Arch::ASM::Mfence();
        
        // Kirim (Response size kasih agak gedean dikit jaga2 balikan DisplayInfo)
        SendCommand(physAddr, sizeof(VirtioGpuCtrlHeader), physAddr + sizeof(VirtioGpuCtrlHeader), 1024);

        // Kita gak perlu cek isinya apa, yang penting antrian QEMU udah 'muntah' event 0x1100 nya.
        // Kalau mau perfect, cek resp->type. Bisa jadi 0x1100 atau 0x1201.
        //Printk::Write(Printk::Level::LOG_DEBUG, "Event Flushed. Type: 0x%x\n", resp->type);
    }

    VOID Driver::SetupScanout(U32 Width, U32 Height){
        this->CurrentWidth = Width;
        U32 Size = Width * Height * 4; 

        // 1. Alokasi VRAM
        PageAlloc::DMAAlloc::DMABuffer *VRAMBuf = PageAlloc::DMAAlloc::AllocateDMABytes(Size);
        if(!VRAMBuf) return;

        // Pattern Merah Putih (Indo pride) buat test
        U32 *PX = (U32*)VRAMBuf->VirtAddr;
        for(U32 i = 0; i < Width * Height; i++) PX[i] = 0xFF00000; // Blue/Red channel check
        
        U8* vramPtr = (U8*)VRAMBuf->VirtAddr;
        for(U32 i = 0; i < Size; i += 64) Arch::ASM::Clflush(vramPtr + i);
        Arch::ASM::Mfence();

        // --------------------------------------------------------
        // ALOKASI TERPISAH (ANTI-OVERLAP)
        // --------------------------------------------------------
        // Buang buffer lama kalau ada
        if (this->CmdBuffer) PageAlloc::DMAAlloc::FreeDMABuffer(this->CmdBuffer);
        
        // Alokasi 2 Buffer terpisah
        PageAlloc::DMAAlloc::DMABuffer *CmdBuf = PageAlloc::DMAAlloc::AllocateDMABytes(4096);
        PageAlloc::DMAAlloc::DMABuffer *RespBuf = PageAlloc::DMAAlloc::AllocateDMABytes(4096);
        
        // Simpan pointer global biar ga leak (opsional, sesuaikan dgn class lu)
        this->CmdBuffer = CmdBuf; 
        // Note: Idealnya simpan RespBuf di class juga, tapi utk skrg local var ok asal di-free nanti.

        U8* cmdBase = (U8*)CmdBuf->VirtAddr;
        U64 cmdPhysBase = CmdBuf->PhysAddr;
        
        U8* respBase = (U8*)RespBuf->VirtAddr;
        U64 respPhysBase = RespBuf->PhysAddr;

        // Bersihkan TOTAL kedua buffer
        String::Memset(cmdBase, 0, 4096);
        String::Memset(respBase, 0, 4096);
        Arch::ASM::Mfence();

        // ------------------------------------------
        // 1. CREATE 2D RESOURCE
        // ------------------------------------------
        U32 retryCount = 0;
        bool success = false;
        
        do {
            // Kita tetap geser offset CMD buat hindari stale cache
            // TAPI, Response selalu di buffer terpisah yang aman (offset 0 di RespBuf)
            U32 cmdOffset = retryCount * 256; 
            
            U8* cmdPtr = cmdBase + cmdOffset;
            U64 physCmd = cmdPhysBase + cmdOffset;
            
            // Response selalu pakai RespBuf dari awal. 
            // Kita reset RespBuf tiap kali retry biar bersih.
            String::Memset(respBase, 0, 1024);
            Arch::ASM::Mfence();

            VirtioGpuCtrlHeader* resp = (VirtioGpuCtrlHeader*)respBase;
            VirtioGpuResourceCreate2D* cmdCreate = (VirtioGpuResourceCreate2D*)cmdPtr;

            // Isi Command
            cmdCreate->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D; // 0x0101
            cmdCreate->resource_id = 1; 
            cmdCreate->format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
            cmdCreate->width = Width;
            cmdCreate->height = Height;
            
            // Flush Command Area
            for(U32 i=0; i<64; i+=64) Arch::ASM::Clflush(cmdPtr + i);
            Arch::ASM::Mfence();
            
            // KIRIM!
            // Perhatikan: Size Resp buffer kita kasih FULL 1024 bytes biar QEMU puas curhat
            if(!SendCommand(physCmd, sizeof(*cmdCreate), respPhysBase, 1024)) return;

            // CEK RESPONSE
            if (resp->type == 0x1100) { 
                success = true; 
                break; 
            }
            // 0x12xx = ERROR
            else if (resp->type >= 0x1200) { 
                Printk::Write(Printk::Level::LOG_DERR, "Virtio Error! Type: 0x%x\n", resp->type);
                retryCount++;
            }
            // 0x1101 = Display Info (Kalo beneran dapet data display)
            else if (resp->type == 0x1101) {
                 retryCount++;
                 for(int k=0; k<10000; k++) Arch::ASM::CPURelax();
            }
            else {
                // Unknown response
                Printk::Write(Printk::Level::LOG_WARNING, "Unknown Response: 0x%x\n", resp->type);
                retryCount++;
            }
        } while (retryCount < 5);

        if (!success) {
            Printk::Write(Printk::Level::LOG_EMERG, "Failed to Create2D.\n");
            // Jangan lupa free RespBuf kalau gagal total
            PageAlloc::DMAAlloc::FreeDMABuffer(RespBuf);
            return;
        }

        // ------------------------------------------
        // 2. ATTACH BACKING
        // ------------------------------------------
        // Pakai slot CMD baru (misal offset 2048) biar bersih
        U32 attachOffset = 2048;
        U8* cmdPtr = cmdBase + attachOffset;
        U64 physCmd = cmdPhysBase + attachOffset;
        
        VirtioGpuResourceAttachBacking* cmdAttach = (VirtioGpuResourceAttachBacking*)cmdPtr;
        VirtioGpuMemEntry* entry = (VirtioGpuMemEntry*)(cmdPtr + sizeof(VirtioGpuResourceAttachBacking));
        
        String::Memset(respBase, 0, 1024); // Reset RespBuf
        
        cmdAttach->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
        cmdAttach->resource_id = 1;
        cmdAttach->nr_entries = 1; 
        entry->addr = VRAMBuf->PhysAddr; 
        entry->length = Size;

        Arch::ASM::Mfence(); // Pastikan struct tertulis
        
        // Kirim
        SendCommand(physCmd, sizeof(*cmdAttach) + sizeof(*entry), respPhysBase, 1024);
        VirtioGpuCtrlHeader* resp = (VirtioGpuCtrlHeader*)respBase;

        if (resp->type != 0x1100) Printk::Write(Printk::Level::LOG_DERR, "Attach Err: 0x%x\n", resp->type);

        // ------------------------------------------
        // 3. SET SCANOUT
        // ------------------------------------------
        U32 scanOffset = 2560;
        cmdPtr = cmdBase + scanOffset;
        physCmd = cmdPhysBase + scanOffset;
        VirtioGpuSetScanout* cmdScanout = (VirtioGpuSetScanout*)cmdPtr;

        String::Memset(respBase, 0, 1024);
        cmdScanout->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
        cmdScanout->resource_id = 1;
        cmdScanout->scanout_id = 0; 
        cmdScanout->r.width = Width;
        cmdScanout->r.height = Height;

        Arch::ASM::Mfence();
        SendCommand(physCmd, sizeof(*cmdScanout), respPhysBase, 1024);
        
        if (resp->type != 0x1100) Printk::Write(Printk::Level::LOG_DERR, "Scanout Err: 0x%x\n", resp->type);

        // ------------------------------------------
        // 4. TRANSFER & FLUSH
        // ------------------------------------------
        U32 transOffset = 3072;
        cmdPtr = cmdBase + transOffset;
        physCmd = cmdPhysBase + transOffset;
        VirtioGpuTransferToHost2D* cmdTrans = (VirtioGpuTransferToHost2D*)cmdPtr;

        String::Memset(respBase, 0, 1024);
        cmdTrans->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
        cmdTrans->resource_id = 1;
        cmdTrans->r.width = Width; cmdTrans->r.height = Height;
        cmdTrans->offset = 0; 

        Arch::ASM::Mfence();
        SendCommand(physCmd, sizeof(*cmdTrans), respPhysBase, 1024);

        // -- RESOURCE FLUSH --
        U32 flushOffset = 3584;
        cmdPtr = cmdBase + flushOffset;
        physCmd = cmdPhysBase + flushOffset;
        VirtioGpuResourceFlush* cmdFlush = (VirtioGpuResourceFlush*)cmdPtr;
        
        String::Memset(respBase, 0, 1024);
        cmdFlush->hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
        cmdFlush->resource_id = 1;
        cmdFlush->r.width = Width; cmdFlush->r.height = Height;

        Arch::ASM::Mfence();
        SendCommand(physCmd, sizeof(*cmdFlush), respPhysBase, 1024);

        FB::ConsoleConfig config;
        config.FrameBufferAddr = VRAMBuf->PhysAddr;
        config.Width = Width;
        config.Height = Height;
        config.Bpp = 32;
        config.Pitch = Width * 4;
        config.OnFlush = &Driver::HardwareFlush;
        Printk::Write(Printk::Level::LOG_INFO, "QEMU: Resolution set at %d, %d", Width, Height);

        FB::Reconfigure(&config);
        
        // Clean up temporary RespBuf (CmdBuf disimpan di class)
        PageAlloc::DMAAlloc::FreeDMABuffer(RespBuf);
    }
}