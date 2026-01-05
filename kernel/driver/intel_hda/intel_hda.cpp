#include "intel_hda.hpp"
#include "../pci/pci.hpp" // Sesuaikan path pci.hpp lu
#include "string.hpp"
#include <mm.hpp>         // Untuk HHDM_PhysToVirt
#include <logging.hpp>

namespace IntelHDA{
    using namespace Printk;
    HDAController GlobalController;
    
    VOIDFUNC HDAController::Initialize(U8 bus, U8 Dev, U8 Func){
        this->Bus = bus;
        this->Device = Dev;
        this->Function = Func;

        Write(Level::LOG_INFO, "[HDA] Initializing HDA Controller at %d:%d:%d...\n", bus, Dev, Func);

        U32 PCICMD = PCI::ReadDword(bus, Dev, Func, 0x04);
        if(!(PCICMD & (1 << 2))){
            PCI::WriteDword(bus, Dev, Func, 0x04, PCICMD | (1 << 2));
            Write(Level::LOG_INFO, "[HDA] PCI Bus Master Enabled.\n");
        }

        U32 BAR0Low = PCI::ReadDword(bus, Dev, Func, 0x10);
        U32 BAR0High = PCI::ReadDword(bus, Dev, Func, 0x14);

        this->MMIOBasePhys = ((U64)BAR0High << 32) | (BAR0Low & ~0xF);
        // ALAMAT mmio pasti tidak masuk akan dan gak mungkin HHDM
        // Pake Virtual allocation
        UPTR VirtualPage = (UPTR)PageAlloc::VirtualAllocPages(32);
        if(!VirtualPage) return;
        if(!PageAlloc::MapPages(KernelPML4, 
            this->MMIOBasePhys, 
            VirtualPage, 
            32, 
            PAGE_PRESENT | PAGE_RW | PAGE_PCD)){
                return;
            }
        this->MMIOBaseVirt = VirtualPage;

        Write(Level::LOG_INFO, "[HDA] MMIO Phys: %lx -> Virt: %lx\n", MMIOBasePhys, MMIOBaseVirt);

        if (!ResetController()) {
            Write(Level::LOG_ERR, "[HDA] FATAL: Controller Reset Failed!\n");
            return;
        }

        if (!InitCORB()) {
           Write(Level::LOG_ERR, "[HDA] Failed to init CORB!\n");
           return;
        }
        
        if (!InitRIRB()) {
            Write(Level::LOG_ERR, "[HDA] Failed to init RIRB!\n");
            return;
        }
        
        Write(Level::LOG_INFO, "[HDA] Step 2 Complete: DMA Rings Setup OK.\n");

        // Cek versi HDA (Optional, buat mastiin bacaan bener)
        U8 vMaj = Read8(HDA_REG_VMAJ);
        U8 vMin = Read8(HDA_REG_VMIN);
        Write(Level::LOG_INFO, "[HDA] Hardware Version: %d.%d\n", vMaj, vMin);
        
        ScanCodec();
        
        Write(Level::LOG_INFO, "[HDA] Step 3 Complete: Codec Scanned.\n");
    }

    BOOLFUNC HDAController::ResetController(){
        U32 gctl = Read32(HDA_REG_GCTL);
        Write32(HDA_REG_GCTL, gctl & ~1);

        // 2. Tunggu sampai hardware membaca bit tersebut sbg 0
        int timeout = 1000;
        while ((Read32(HDA_REG_GCTL) & 1) && timeout-- > 0) {
            Arch::Time::SleepMs(1); // Pakai fungsi sleep dari rossys.hpp
        }
        if (timeout <= 0) {
            Write(Level::LOG_ERR, "[HDA] Timeout waiting for Reset (Enter)!\n");
            return FALSE;
        }

        // 3. Write 1 ke bit 0 (CRST) GCTL untuk menyalakan kembali
        gctl = Read32(HDA_REG_GCTL);
        Write32(HDA_REG_GCTL, gctl | 1);

        // 4. Tunggu sampai hardware siap (bit 0 jadi 1)
        timeout = 1000;
        while (!(Read32(HDA_REG_GCTL) & 1) && timeout-- > 0) {
            Arch::Time::SleepMs(1);
        }
        
        if (timeout <= 0) {
            Write(Level::LOG_ERR, "[HDA] Timeout waiting for Reset (Exit)!\n");
            return FALSE;
        }

        // Tunggu sedikit lagi biar sinyal clock ke Codec stabil
        Arch::Time::SleepMs(100); 
        return TRUE;
    }

    BOOLFUNC HDAController::InitCORB(){
        Write8(HDA_REG_CORBCTL, 0);

        auto dmaBuf = PageAlloc::DMAAlloc::AllocateDMAPages(1);
        if (!dmaBuf) return FALSE;

        UPTR UncachedVirt = (UPTR)PageAlloc::VirtualAllocPages(1);
        
        // Map Physical Address dari dmaBuf ke Virtual Address baru ini dengan PAGE_PCD
        if(!PageAlloc::MapPages(KernelPML4, 
            dmaBuf->PhysAddr, 
            UncachedVirt, 
            1, 
            PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_GLOBAL)){ // Tambah PAGE_GLOBAL biar aman di TLB
             return FALSE;
        }

        this->CORBBuffer = (U32*)UncachedVirt;

        String::Memset((VOID*)this->CORBBuffer, 0, 4096);

        U64 PhysAddr = dmaBuf->PhysAddr;
        Write32(HDA_REG_CORBLBASE, (U32)(PhysAddr & 0xFFFFFFFF));
        Write32(HDA_REG_CORBUBASE, (U32)(PhysAddr >> 32));

        U8 sizeCap = Read8(HDA_REG_CORBSIZE);
        if ((sizeCap & 0xF0) & 0x40) {
            Write8(HDA_REG_CORBSIZE, 0x02);
        } else {
            // Force 256 aja, jarang banget ada chip modern gak support
            Write8(HDA_REG_CORBSIZE, 0x02);
        }
        this->CORBEntries = 256;

        Write16(HDA_REG_CORBRP, 0x8000);
        int timeout = 1000;
        while(!(Read16(HDA_REG_CORBRP) & 0x8000) && timeout-- > 0) Arch::Time::SleepMs(1);

        Write16(HDA_REG_CORBRP, 0x0000);
        timeout = 1000;
        while((Read16(HDA_REG_CORBRP) & 0x8000) && timeout-- > 0) Arch::Time::SleepMs(1);

        Write16(HDA_REG_CORBWP, 0);
        
        // Reset pointer software kita
        this->CORBWritePtr = 0;

        // 7. Nyalakan CORB DMA (Set Bit 1)
        Write8(HDA_REG_CORBCTL, 0x02);

        return TRUE;
    }

    BOOLFUNC HDAController::InitRIRB(){
        Write8(HDA_REG_RIRBCTL, 0);

        auto dmaBuf = PageAlloc::DMAAlloc::AllocateDMAPages(1);
        if (!dmaBuf) return FALSE;

        UPTR UncachedVirt = (UPTR)PageAlloc::VirtualAllocPages(1);
        if(!PageAlloc::MapPages(KernelPML4, 
            dmaBuf->PhysAddr, 
            UncachedVirt, 
            1, 
            PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_GLOBAL)){
             return FALSE;
        }
        this->RIRBBuffer = (U64*)UncachedVirt;
        U64 physAddr = dmaBuf->PhysAddr;
        String::Memset((void*)this->RIRBBuffer, 0, 4096);

        Write32(HDA_REG_RIRBLBASE, (U32)(physAddr & 0xFFFFFFFF));
        Write32(HDA_REG_RIRBUBASE, (U32)(physAddr >> 32));

        Write8(HDA_REG_RIRBSIZE, 0x02);
        this->RIRBEntries = 256;

        Write16(HDA_REG_RIRBWP, 0x8000);

        // Setup Interrupt (Opsional dulu, tapi bagus diset)
        //    Interrupt setiap 1 response masuk (N=1)
        Write16(HDA_REG_RIRBINTCNT, 1);

        // Reset pointer baca software kita
        this->RIRBReadPtr = 0; 

        // 7. Nyalakan RIRB DMA (Bit 1 = DMA Enable, Bit 0 = IRQ Enable)
        //    Kita nyalakan DMA dulu (0x02). Kalau IRQ handler udah siap nanti bisa 0x03.
        Write8(HDA_REG_RIRBCTL, 0x02);

        return TRUE;
    }

    DWORD HDAController::SendVerb(BYTE CAD, BYTE NID, DWORD Payload){
        DWORD Verb = ((DWORD)CAD << 28) | ((DWORD)NID << 20) | (Payload & 0xFFFFF);

        // 1. Snapshot Posisi Awal
        WORD OldRirbWP = Read16(HDA_REG_RIRBWP);
        WORD CurrentHW_WP = Read16(HDA_REG_CORBWP);

        // 2. Hitung Target Slot
        WORD NextWritePtr = (CurrentHW_WP + 1) % this->CORBEntries;

        // 3. --- PARANOID WRITE ---
        // Tulis ke memori
        this->CORBBuffer[NextWritePtr] = Verb;
        asm volatile("mfence" ::: "memory");
        
        // BACA BALIK! Pastikan CPU gak bohong.
        // Kalau kita baca balik, CPU terpaksa memastikan jalur data valid.
        volatile DWORD CheckData = this->CORBBuffer[NextWritePtr];
        if (CheckData != Verb) {
             Write(Level::LOG_ERR, "[HDA] RAM WRITE FAILED! Wrote %x Read %x. Retrying...\n", Verb, CheckData);
             this->CORBBuffer[NextWritePtr] = Verb;
             asm volatile("mfence" ::: "memory");
        }

        // 4. --- AGGRESSIVE DOORBELL ---
        // Kita loop nunggu RP gerak. Kalau gak gerak dalam 10ms, kita TENDANG LAGI.
        Write16(HDA_REG_CORBWP, NextWritePtr);
        
        INT32 FetchTimeout = 100; // 100ms total
        BOOL Fetched = FALSE;
        
        while(FetchTimeout > 0){
            WORD CurrentRP = Read16(HDA_REG_CORBRP);
            
            if(CurrentRP == NextWritePtr){
                Fetched = TRUE;
                break;
            }
            
            // Trik buat QEMU: Kalau udah 10ms masih macet, tulis ulang WP-nya!
            // Kadang interrupt internal QEMU miss.
            if (FetchTimeout % 10 == 0) {
                 Write16(HDA_REG_CORBWP, NextWritePtr);
            }

            Arch::Time::SleepMs(1);
            FetchTimeout--;
        }

        if(!Fetched){
             // Cek isi memori dari kacamata debug, apakah beneran 0?
             Write(Level::LOG_ERR, "[HDA] FETCH ERROR! RP=%d Target=%d | RAM[%d]=%x\n", 
                Read16(HDA_REG_CORBRP), NextWritePtr, NextWritePtr, this->CORBBuffer[NextWritePtr]);
             return 0xFFFFFFFF;
        }

        // 5. --- TUNGGU RESPONSE ---
        INT32 ResponseTimeout = 100; 
        while(ResponseTimeout > 0) {
            WORD CurrentRirbWP = Read16(HDA_REG_RIRBWP);
            
            if (CurrentRirbWP != OldRirbWP) {
                this->RIRBReadPtr = CurrentRirbWP;
                
                // Flush RIRB (just in case)
                asm volatile("mfence" ::: "memory");
                
                QWORD ResponseFull = this->RIRBBuffer[CurrentRirbWP];
                Write8(HDA_REG_RIRBSTS, 0x05); // Ack
                
                return (DWORD)(ResponseFull & 0xFFFFFFFF);
            }
            Arch::Time::SleepMs(1);
            ResponseTimeout--;
        }

        Write(Level::LOG_ERR, "[HDA] COMMAND FETCHED BUT NO RESPONSE!\n"); 
        return 0xFFFFFFFF;
    }

    VOIDFUNC HDAController::ScanCodec() {
        Write(Level::LOG_INFO, "[HDA] Scanning for Codecs...\n");
        
        // Bitmask Codec yang aktif ada di Register STATETS (Offset 0x0E)
        U16 statests = Read16(HDA_REG_STATESTS);
        
        // Loop 15 kemungkinan slot address
        for (int i = 0; i < 15; i++) {
            // Cek apakah bit ke-i nyala? (Artinya ada sinyal dari codec no-i)
            if ((statests >> i) & 1) {
                // Konfirmasi dengan kirim perintah "GET PARAMETER (VENDOR ID)"
                // Verb ID: 0xF00, Param: 0x00 (Vendor ID)
                // Jadi Payload = 0xF0000
                U32 resp = SendVerb(i, 0, 0xF0000);

                if (resp != 0xFFFFFFFF && resp != 0) {
                    U16 vendor = resp >> 16;
                    U16 device = resp & 0xFFFF;
                    Write(Level::LOG_INFO, "[HDA] Found Codec at Address %d: VenID=%x DevID=%x\n", 
                        i, vendor, device);
                    
                    ParseWidgets(i);
                }
            }
        }
    }

    NORESULTFUNC HDAController::ParseWidgets(BYTE CAD){
        DWORD Param = SendVerb(CAD, 0, 0xF0004);
        if (Param == 0xFFFFFFFF) {
            Write(Level::LOG_ERR, "[HDA] Failed to get root node info for Codec %d\n", CAD);
            return;
        }
        WORD StartNode = (Param >> 16) & 0xFF;
        U16 TotalNode = Param & 0xFF;

        Write(Level::LOG_INFO, "[HDA] Codec %d Info: StartNode=%d TotalNodes=%d\n", CAD, StartNode, TotalNode);

        for(INT32 i = 0; i < TotalNode; i++){
            U16 NID = StartNode + i;
            U32 FuncType = SendVerb(CAD, NID, 0xF0005);

            if((FuncType & 0xFF) == 0x01){
                Write(Level::LOG_INFO, "[HDA] Found Audio Function Group at NID %d\n", NID);

                U32 WidgetInfo = SendVerb(CAD, NID, 0xF0004);
                U16 WidgetStart = (WidgetInfo >> 16) & 0xFF;
                U16 WidgetCount = WidgetInfo & 0xFF;

                Write(Level::LOG_INFO, "[HDA] -- Scanning %d Widgets starting at %d --\n", WidgetCount, WidgetStart);

                for(INTN j = 0; j < WidgetCount; j++){
                    U16 WID = WidgetStart + j;

                    U32 Caps = SendVerb(CAD, WID, 0xF0009);
                    INT32 Type = (Caps >> 20) & 0xF;

                    CONSTANT CHAR8* typeStr = "Unknown";
                    switch(Type) {
                        case WIDGET_AUDIO_OUTPUT: typeStr = "Audio Output (DAC)"; break;
                        case WIDGET_AUDIO_INPUT:  typeStr = "Audio Input (ADC)"; break;
                        case WIDGET_MIXER:        typeStr = "Mixer"; break;
                        case WIDGET_SELECTOR:     typeStr = "Selector"; break;
                        case WIDGET_PIN_COMPLEX:  typeStr = "Pin Complex (Jack)"; break;
                        case WIDGET_POWER:        typeStr = "Power Widget"; break;
                    }

                    Write(Level::LOG_INFO, "[HDA] Widget NID %d: %s\n", WID, typeStr);

                    if (Type == WIDGET_PIN_COMPLEX) {
                         __MAYBE_UNUSED U32 Config = SendVerb(CAD, WID, 0xF1C00); // Get Configuration Default
                         // Nanti kita parse ini di Step 5
                    }
                }
            }
        }
    }
}