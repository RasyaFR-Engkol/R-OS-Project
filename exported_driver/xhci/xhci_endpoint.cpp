#include <rosval.h>
#include "xhci.hpp"
#include "xhci_regs.hpp"
#include "xhci_internal.hpp"
#include <string.hpp>
#include <logging.hpp>
#include "../massusb/usb_defs.hpp"

namespace xHCI{
    U32 CalcDCI(U8 EpAddr){
        U8 EpNum = EpAddr & 0x0F;
        U8 EpDir = (EpAddr & 0x80) ? 1 : 0; // 1=IN, 0=OUT

        return (2 * EpNum) + EpDir;
    }

    VOID ConfigureEndpoint(xHCIDriver &DRV, U8 SlotID, U8 EpAddr, U8 EpType, U16 MaxPacketSize, U32 Interval){
        U32 DCI = CalcDCI(EpAddr);
        //Printk::Write(Printk::Level::LOG_INFO, " xHCI: ConfigureEndpoint - Slot %u EpAddr 0x%02x DCI %u Type %u MPS %u Interval %u\n",
        //    (unsigned)SlotID, (unsigned)EpAddr, (unsigned)DCI, (unsigned)EpType, (unsigned)MaxPacketSize, (unsigned)Interval);

        PageAlloc::DMAAlloc::DMABuffer *EPRing = PageAlloc::DMAAlloc::AllocateDMABytes(4096);
        if(!EPRing){
          //  Printk::Write(Printk::Level::LOG_ERR, " ConfigureEndpoint: Failed to allocate ring for Slot %u EpAddr 0x%02x\n",
          //      (unsigned)SlotID, (unsigned)EpAddr);
            return;
        }

        DRV.Devs[SlotID].Endpoints[DCI].Ring = EPRing;
        DRV.Devs[SlotID].Endpoints[DCI].EnqueueIdx = 0;
        DRV.Devs[SlotID].Endpoints[DCI].CycleState = TRUE;

        // init ring memory
        {
            U8 *V = (U8*)EPRing->VirtAddr;
            String::Memset((VOID*)V, 0, EPRing->Size);
            
            // setup link TRB at last entry
            U32 Entries = (U32)(EPRing->Size / sizeof(xHCITRB));
            xHCITRB *Table = (xHCITRB*)EPRing->VirtAddr;
            xHCITRB *Link = &Table[Entries - 1];

            Link->parameter = EPRing->PhysAddr;
            Link->status = 0;
            // TC Toggle Cycle on wrap. Cycle bit should match initial Producer Cycle State (1).
            Link->control = (6U << 10) | (1U << 1) | (DRV.Devs[SlotID].Endpoints[DCI].CycleState ? 1u : 0u);
        }

        // siapkan input context
        PageAlloc::DMAAlloc::DMABuffer *InputCtxDMA = PageAlloc::DMAAlloc::AllocateDMAPages(1);
        if(!InputCtxDMA) return;

        U8* InputCtxBase = (U8*)InputCtxDMA->VirtAddr;
        String::Memset(InputCtxBase, 0, PAGE_SIZE);

        BOOL Is64ByteCtx = (DRV.cap_regs->hcc_params1 & (1 << 2));
        U32 CtxSize = Is64ByteCtx ? 64 : 32;

        // --- POINTER FIX ---
        // Struktur Input Context: [ICC (size=CtxSize)] [Device Context (SlotCtx, EP0, EP1...)]
        // Jadi Slot Context ada di offset CtxSize.
        // Endpoint Context ada di offset CtxSize + (DCI * CtxSize).
        
        volatile U32 *ICC  = (volatile U32*)(InputCtxBase);
        volatile U32 *Slot = (volatile U32*)(InputCtxBase + CtxSize);
        // FIX: Tambahkan CtxSize lagi biar gak numpuk di Slot Context kalau DCI=1
        volatile U32 *EP   = (volatile U32*)(InputCtxBase + CtxSize + (DCI * CtxSize));

        // --- Input Control Context ---
        // Enable Slot Context (bit 0) dan Endpoint Context (bit DCI)
        ICC[1] = (1U << 0) | (1U << DCI);

        // --- Slot Context ---
        // KITA HARUS ISI ULANG INFO SLOT KARENA BUFFER INI KOSONG (0)
        // Kalau RootHubPort/Speed diisi 0, xHCI bisa mutus koneksi.
        
        // Ambil info yang tersimpan (Pastikan kamu set ini pas Address Device!)
        U8 RootPort = DRV.Devs[SlotID].RootPortID; 
        UNUSED__ U8 Speed    = DRV.Devs[SlotID].PortSpeed;

        Slot[0] |= (1 << 20); // Speed bisa dimasukin di sini (bits 23:20) tapi biasanya Context Entries lebih penting
        Slot[0] &= ~(0x1F << 27);
        Slot[0] |= (DCI << 27); // Update Context Entries (Last valid DCI)

        // Penting: Masukkan Root Hub Port Number lagi
        if(RootPort) Slot[1] |= (RootPort << 16);
        
        // Penting: Masukkan Speed (opsional tergantung controller, tapi aman dipasang)
        // Note: Speed biasanya di Address Device, tapi Configure EP kadang butuh validasi ulang.

        // ----------------------------------------
        // SETUP ENDPOINT CONTEXT
        // ----------------------------------------
        U32 EncodedInterval = 7; // 8ms for FS/LS
        // TODO: Logic konversi bInterval asli ke xHCI interval format
        
        EP[0] |= (EncodedInterval << 16);

        // Dword 1: CErr=3, EPType=7, MaxBurst=0, MPS
        EP[1] |= (3 << 1);
        EP[1] |= (EpType << 3); 
        EP[1] |= ((U32)MaxPacketSize << 16);

        // Dword 2: Dequeue Pointer Lo & DCS
        U64 RingPhys = EPRing->PhysAddr;
        EP[2] = (U32)(RingPhys & 0xFFFFFFFF) | 1; // DCS=1

        // Dword 3: Dequeue Pointer Hi
        EP[3] = (U32)(RingPhys >> 32);

        // Dword 4: Average TRB Length
        EP[4] = MaxPacketSize;

        //Printk::Write(Printk::Level::LOG_DEBUG, " xHCI: Sending Configure Endpoint for Slot %u DCI %u\n", SlotID, DCI);

        asm volatile ("mfence" ::: "memory");

        U32 CmdIdx = DRV.CmdRingEnqueueIndex;
        volatile xHCITRB *CmdTRB = &DRV.VCmdRing[CmdIdx];

        CmdTRB->parameter = InputCtxDMA->PhysAddr;
        CmdTRB->status    = 0;
        CmdTRB->control   = (12u << 10) | (SlotID << 24) | (DRV.CmdRingCycleState ? 1u : 0u);

        DRV.doorbell_regs[0] = 0;

        DRV.CmdRingEnqueueIndex++;
        if(DRV.CmdRingEnqueueIndex == DRV.CmdRingSize - 1){
            volatile xHCITRB *LinkTRB = &DRV.VCmdRing[DRV.CmdRingEnqueueIndex];
            LinkTRB->parameter = DRV.DMA_CmdRing->PhysAddr;
            LinkTRB->status = 0;
            LinkTRB->control = (6U << 10) | (1U << 1) | (DRV.CmdRingCycleState ? 1u : 0u);
            
            DRV.CmdRingEnqueueIndex = 0;
            DRV.CmdRingCycleState = !DRV.CmdRingCycleState;
        }
    }

    VOID QueueInterruptTransfer(xHCIDriver &DRV, U8 SlotID, U8 DCI, U64 BufferPhys, U32 Length) {
        if (SlotID >= 255 || DCI >= 32) return;
        
        auto &ep = DRV.Devs[SlotID].Endpoints[DCI];
        if (!ep.Ring) {
            Printk::Write(Printk::Level::LOG_ERR, " QueueInt: No Ring for Slot %u DCI %u\n", SlotID, DCI);
            return;
        }

        xHCITRB *ringBase = (xHCITRB*)ep.Ring->VirtAddr;
        U32 entries = (U32)(ep.Ring->Size / sizeof(xHCITRB));
        U32 idx = ep.EnqueueIdx;

        // Handle Link TRB Wrap
        if (idx == entries - 1) {
            xHCITRB *link = &ringBase[idx];
            // Toggle Cycle (TC) bit harus diset biar controller tau dia harus flip cycle bit internalnya
            link->control = (6u << 10) | (1u << 1) | (ep.CycleState ? 1u : 0u);
            idx = 0;
            ep.CycleState = !ep.CycleState;
        }

        xHCITRB *trb = &ringBase[idx];
        trb->parameter = BufferPhys;
        trb->status    = Length; // Transfer Length
        
        // IOC=1 (Interrupt), ISP=1 (Short Packet OK), Type=1 (Normal)
        trb->control   = (1u << 10) | (1u << 5) | (1u << 2) | (ep.CycleState ? 1u : 0u);

        asm volatile("clflush (%0)" :: "r"(trb) : "memory");
        asm volatile("mfence" ::: "memory");

        ep.EnqueueIdx = idx + 1;

        DRV.doorbell_regs[SlotID] = DCI;
    }

    VOID QueueBulkTransfer(xHCIDriver &DRV, U8 SlotID, U8 DCI, U64 BufferPhys, U32 Length) {
        if (SlotID >= 255 || DCI >= 32) return;
        
        auto &ep = DRV.Devs[SlotID].Endpoints[DCI];
        if (!ep.Ring) {
            Printk::Write(Printk::Level::LOG_ERR, " QueueInt: No Ring for Slot %u DCI %u\n", SlotID, DCI);
            return;
        }

        xHCITRB *ringBase = (xHCITRB*)ep.Ring->VirtAddr;
        U32 entries = (U32)(ep.Ring->Size / sizeof(xHCITRB));
        U32 idx = ep.EnqueueIdx;

        // Handle Link TRB Wrap
        if (idx == entries - 1) {
            xHCITRB *link = &ringBase[idx];
            // Toggle Cycle (TC) bit harus diset biar controller tau dia harus flip cycle bit internalnya
            link->control = (6u << 10) | (1u << 1) | (ep.CycleState ? 1u : 0u);

            asm volatile("clflush (%0)" :: "r"(link) : "memory");
            asm volatile("mfence" ::: "memory");

            idx = 0;
            ep.CycleState = !ep.CycleState;
        }

        xHCITRB *trb = &ringBase[idx];
        trb->parameter = BufferPhys;
        trb->status    = Length; // Transfer Length
        
        // IOC=1 (Interrupt), ISP=1 (Short Packet OK), Type=1 (Normal)
        trb->control   = (1u << 10) | (1u << 5) | (ep.CycleState ? 1u : 0u);

        asm volatile("clflush (%0)" :: "r"(trb) : "memory");    
        asm volatile("mfence" ::: "memory");

        ep.EnqueueIdx = idx + 1;
        ep.CycleState = ep.CycleState; // Tetap sama untuk Bulk

        DRV.doorbell_regs[SlotID] = DCI;
    }
}