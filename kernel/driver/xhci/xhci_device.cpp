#include <rosval.h>
#include "xhci_internal.hpp"
#include "xhci.hpp"
#include "xhci_isr.hpp"
#include "xhci_regs.hpp"
#include <string.hpp>
#include <logging.hpp>
#include "../massusb/usb_defs.hpp"

namespace xHCI{
    static U32 GetMaxPacketSize(U32 PortSpeed) {
        // PortSpeed value dari Protocol Speed ID (PSI)
        // Simplifikasi standar: 
        // 4 = SuperSpeed (512 bytes)
        // 3 = HighSpeed (64 bytes)
        // FullSpeed = 8 or 64. LowSpeed = 8.
        // Lo harus parsing register PORTSC (bits 13:10 -> Port Speed)
        
        // Cek spec xHCI section 4.3 (USB Device Initialization)
        if (PortSpeed == 4) return 512; // SuperSpeed
        if (PortSpeed == 3) return 64;  // HighSpeed
        return 8; // Default safe value buat Full/Low speed sebelum baca descriptor
    }

    VOID SetupAddressDevice(xHCIDriver &DRV, U8 SlotID, U8 RootPortID){
        U64 DeviceContextPhys = PageAlloc::PhysicalAllocPages(PAGE_SIZE);
        DRV.V_DCBAAP[SlotID] = DeviceContextPhys;
        // Ensure the DCBAAP entry is visible to the host before issuing commands.
        // Flush the DCBAAP entry out of CPU caches so the controller can read it.
        asm volatile ("clflush (%0)" :: "r"(&DRV.V_DCBAAP[SlotID]) : "memory");
        asm volatile ("mfence" ::: "memory");

        String::Memset((VOID*)HHDM_PhysToVirt((UPTR)DeviceContextPhys), 0, PAGE_SIZE);
        // Flush the newly-zeroed Device Context page so HC doesn't see stale data.
        {
            UPTR dc_start = (UPTR)HHDM_PhysToVirt((UPTR)DeviceContextPhys);
            for (UPTR p = dc_start; p < dc_start + PAGE_SIZE; p += 64) {
                asm volatile("clflush (%0)" :: "r"((void*)p) : "memory");
            }
            asm volatile ("mfence" ::: "memory");
        }
        // Debug: verify DCBAAP write/readback and controller op_regs DCBAAP
        {
            U64 rb = DRV.V_DCBAAP[SlotID];
            U64 op_dcbaap = DRV.op_regs->dcbaap;
            Write(Printk::Level::LOG_INFO, " DCBAAP write check: slot=%u wrote=0x%016llx readback=0x%016llx op_dcbaap=0x%016llx\n",
                  (unsigned)SlotID, (unsigned long long)DeviceContextPhys, (unsigned long long)rb, (unsigned long long)op_dcbaap);
            // Dump first few DCBAAP entries (virt) to help inspect what's in DMA buffer
            if (DRV.V_DCBAAP) {
                for (int di = 0; di < 4; ++di) {
                    U64 v = DRV.V_DCBAAP[di];
                    Write(Printk::Level::LOG_INFO, " DCBAAP[%d]=0x%016llx\n", di, (unsigned long long)v);
                }
            }
            // Flush the entire DCBAAP DMA buffer so controller sees updates
            if (DRV.DMA_DCBAAP) {
                UPTR start = (UPTR)DRV.DMA_DCBAAP->VirtAddr;
                UPTR end = start + DRV.DMA_DCBAAP->Size;
                for (UPTR p = start; p < end; p += 64) {
                    asm volatile("clflush (%0)" :: "r"((void*)p) : "memory");
                }
                asm volatile ("mfence" ::: "memory");
            }
        }

        

        U64 InputContextPhys = PageAlloc::PhysicalAllocPages(PAGE_SIZE);
        VOLATILE U32 *InputContextBase = (volatile U32*)HHDM_PhysToVirt(InputContextPhys);

        String::Memset((VOID*)InputContextBase, 0, PAGE_SIZE);

        // 1. DETEKSI CONTEXT SIZE (32 vs 64 Bytes)
        // Bit 2 di HCCPARAMS1 menentukan Context Size (CSZ). 
        // 1 = 64 bytes, 0 = 32 bytes.
        BOOL Is64ByteCtx = (DRV.cap_regs->hcc_params1 & (1 << 2));
        U32 CtxSize = Is64ByteCtx ? 64 : 32;

        // Setup Input Control Context Header (Selalu di awal / index 0)
        InputContextBase[1] = (1 << 0) | (1 << 1); // Valid: Slot Context & EP0

        // 2. HITUNG POINTER DENGAN BENAR
        // Input Context Layout:
        // Index 0: Input Control Context
        // Index 1: Slot Context
        // Index 2: Endpoint 0 Context
        // Kita pakai byte arithmetic (U8*) biar akurat.
        U8* BasePtr = (U8*)InputContextBase;
        
        VOLATILE U32 *SlotContext = (VOLATILE U32*)(BasePtr + (1 * CtxSize));
        VOLATILE U32 *Ep0Context  = (VOLATILE U32*)(BasePtr + (2 * CtxSize));

        U32 PortSC = DRV.port_regs[RootPortID - 1].port_sc;
        U32 Speed = (PortSC >> 10) & 0xF;

        // Setup Slot Context
        // Word 0: Route=0, Speed, Context Entries=1 (buat aktifin EP0)
        SlotContext[0] = 0;
        SlotContext[0] |= (1 << 27);
        SlotContext[0] |= (Speed << 20);

        // Word 1: Root Hub Port Num
        SlotContext[1] |= (RootPortID << 16);
        
        // Word 2: Interrupter Target 0
        SlotContext[2] |= (0 << 22); 

        // Setup EP0 Context (endpoint characteristics are in dword 0)
        U32 MaxPaketSize = GetMaxPacketSize(Speed);

        Ep0Context[0] |= (3 << 1); // Error Count = 3
        Ep0Context[0] |= (4 << 3); // EP Type = Control
        Ep0Context[0] |= (MaxPaketSize << 16);

        // Debug: dump the populated Input Context dwords (helpful for controller troubleshooting)
        {
            volatile U32 *inp = (volatile U32*)HHDM_PhysToVirt((UPTR)InputContextPhys);
            Write(Printk::Level::LOG_INFO, " InputContext populated (slot=%u):\n", (unsigned)SlotID);
            for (int ii = 0; ii < (int)(CtxSize/4 * 3); ++ii) {
                if (ii >= 16) break; // limit spam
                Write(Printk::Level::LOG_INFO, "  IC[%d]=0x%08x\n", ii, (unsigned)inp[ii]);
            }
        }

        // Flush the Input Context memory so the host controller sees the full
        // contents (write-back + invalidate). Walk by cache line size (64).
        {
            UPTR ic_start = (UPTR)InputContextBase;
            UPTR ic_end = ic_start + (CtxSize * 3);
            for (UPTR p = ic_start; p < ic_end; p += 64) {
                asm volatile("clflush (%0)" :: "r"((void*)p) : "memory");
            }
            asm volatile ("mfence" ::: "memory");
        }

        PageAlloc::DMAAlloc::DMABuffer *EP0Ring = PageAlloc::DMAAlloc::AllocateDMABytes(4096);
        if(!EP0Ring){
            Printk::Write(Printk::Level::LOG_ERR, " Failed allocating EP0 Ring\n");
            return;
        }
        U64 EP0RingPhys = EP0Ring->PhysAddr;

        // Runtime State update
        DRV.Devs[SlotID].EP0Ring = EP0Ring;
        DRV.Devs[SlotID].EP0EnqueueIdx = 0;
        DRV.Devs[SlotID].EP0CycleState = TRUE; 

        // Initialize ring memory and place Link TRB at last entry so hw can wrap
        {
            PageAlloc::DMAAlloc::DMABuffer *r = EP0Ring;
            U8 *v = (U8*)r->VirtAddr;
            String::Memset((VOID*)v, 0, r->Size);
            U32 entries = (U32)(r->Size / sizeof(xHCITRB));
            if (entries > 1) {
                xHCITRB *table = (xHCITRB*)r->VirtAddr;
                xHCITRB *link = &table[entries - 1];
                link->parameter = r->PhysAddr;
                link->status = 0;
                link->control = (6U << 10) | (1U << 1) | (DRV.Devs[SlotID].EP0CycleState ? 1u : 0u);
                // Make sure the link TRB (and ring page) is visible to the host.
                for (UPTR p = (UPTR)r->VirtAddr; p < (UPTR)r->VirtAddr + r->Size; p += 64) {
                    asm volatile("clflush (%0)" :: "r"((void*)p) : "memory");
                }
                asm volatile ("mfence" ::: "memory");
            }
        }

        // Dequeue Pointer & DCS
        Ep0Context[2] = (U32)(EP0RingPhys & 0xFFFFFFFF) | 1;
        Ep0Context[3] = (U32)(EP0RingPhys >> 32);
        
        // Avg TRB Length
        Ep0Context[4] = (8 << 0);

        Printk::Write(Printk::Level::LOG_DEBUG, " xHCI: Sending Address Device Slot %u Port %u (CtxSize: %u)\n",
            (unsigned)SlotID, (unsigned)RootPortID, (unsigned)CtxSize);

        // --- COMMAND SUBMISSION LOGIC (Udah bener) ---
        U32 Index = DRV.CmdRingEnqueueIndex;
        VOLATILE xHCITRB *CmdTRB = &DRV.VCmdRing[Index];

        CmdTRB->parameter = InputContextPhys;
        CmdTRB->status = 0;
        CmdTRB->control = (11u << 10) | (SlotID << 24) | (DRV.CmdRingCycleState ? 1u : 0u);
        // Ensure the command TRB is written back to memory before the host reads it.
        asm volatile("clflush (%0)" :: "r"(CmdTRB) : "memory");
        asm volatile ("mfence" ::: "memory");

        DRV.doorbell_regs[0] = 0; 

        DRV.CmdRingEnqueueIndex++;
        if(DRV.CmdRingEnqueueIndex == DRV.CmdRingSize - 1){
            VOLATILE xHCITRB *LinkTRB = &DRV.VCmdRing[DRV.CmdRingEnqueueIndex];
            LinkTRB->parameter = DRV.DMA_CmdRing->PhysAddr;
            LinkTRB->status = 0;
            LinkTRB->control = (6U << 10) | (1U << 1) | (DRV.CmdRingCycleState ? 1u : 0u); 
            DRV.CmdRingEnqueueIndex = 0;
            DRV.CmdRingCycleState = !DRV.CmdRingCycleState;
        }
    }

    VOID PushEP0TRB(xHCIDriver &DRV, U8 SlotID, xHCITRB &TRB){
        // Ambil pointer ring EP0 device ini (dari struct device management lo)
        // Anggap: DRV.Devs[SlotID].EP0Ring
        // Dan index: DRV.Devs[SlotID].EP0EnqueueIdx
        // Dan cycle: DRV.Devs[SlotID].EP0CycleState
        
        if(SlotID == 0 || (U32)SlotID >= xHCIDriver::MAX_SLOTS){
            Printk::Write(Printk::Level::LOG_ERR, " PushEP0TRB: invalid SlotID %u\n", (unsigned)SlotID);
            return;
        }

        auto &dev = DRV.Devs[SlotID];
        PageAlloc::DMAAlloc::DMABuffer *ring = dev.EP0Ring;
        if(!ring){
            Printk::Write(Printk::Level::LOG_ERR, " PushEP0TRB: EP0 ring not allocated for slot %u\n", (unsigned)SlotID);
            return;
        }

        U32 entryCount = (U32)(ring->Size / sizeof(xHCITRB));
        if(entryCount < 2){
            Printk::Write(Printk::Level::LOG_ERR, " PushEP0TRB: EP0 ring too small (%u entries)\n", (unsigned)entryCount);
            return;
        }

        xHCITRB *table = (xHCITRB*)(ring->VirtAddr);

        // Ensure TRB cycle bit matches current cycle state
        if(dev.EP0CycleState)
            TRB.control |= 1u;
        else
            TRB.control &= ~1u;

        U32 idx = dev.EP0EnqueueIdx;

        if(idx == entryCount - 1){
            xHCITRB *link = &table[idx];
            link->parameter = dev.EP0Ring->PhysAddr; 
            link->status    = 0;
            // Toggle Cycle bit buat Link TRB biar hardware mau baca
            link->control   = (6u << 10) | (1u << 1) | (dev.EP0CycleState ? 1u : 0u);
            // Make sure link TRB is visible to host
            asm volatile("clflush (%0)" :: "r"(link) : "memory");
            asm volatile ("mfence" ::: "memory");

            // Wrap ke awal
            idx = 0;
            dev.EP0CycleState = !dev.EP0CycleState; // Flip cycle bit kita
        }

        xHCITRB *dest = &table[idx];

        // Debug: log TRB push (slot, idx, type)
        Printk::Write(Printk::Level::LOG_INFO, " PushEP0TRB: slot=%u idx=%u control=0x%08x param=0x%016llx\n",
            (unsigned)SlotID, (unsigned)idx, (unsigned)TRB.control, (unsigned long long)TRB.parameter);
        dest->parameter = TRB.parameter;
        dest->status    = TRB.status;

        U32 ctrl = TRB.control;
        if(dev.EP0CycleState) ctrl |= 1u;
        else                  ctrl &= ~1u;

        dest->control = ctrl;

        // Ensure TRB reaches memory (flush the TRB cache line)
        asm volatile("clflush (%0)" :: "r"(dest) : "memory");
        asm volatile ("mfence" ::: "memory");

        dev.EP0EnqueueIdx = idx + 1;
    }

    VOID GetDeviceDescriptor(xHCIDriver &DRV, U8 SlotID){
        if (SlotID == 0) {
            Printk::Write(Printk::Level::LOG_ERR, " GetDeviceDescriptor: invalid SlotID 0\n");
            return;
        }

        PageAlloc::DMAAlloc::DMABuffer *DestBuffer = PageAlloc::DMAAlloc::AllocateDMABytes(sizeof(USBDeviceDescriptor));
        if(!DestBuffer){
            Printk::Write(Printk::Level::LOG_ERR, " Failed allocating buffer for Get Device Descriptor\n");
            return;
        }

        // Remember destination physical address for ISR to inspect on completion
        DRV.Devs[SlotID].LastEP0DestPhys = DestBuffer->PhysAddr;

        Printk::Write(Printk::Level::LOG_DEBUG, " GetDeviceDescriptor: slot=%u destPhys=0x%016llx\n",
            (unsigned)SlotID, (unsigned long long)DestBuffer->PhysAddr);

        xHCITRB SetupTRB;
        // Parameter di Setup TRB itu unik: isinya RAW 8 bytes USB Setup Packet
        // Kita pack manual jadi 2x U32 biar masuk ke field parameter & status
        // Request: 80 06 00 01 00 00 12 00 (Get Device Descriptor, 18 bytes)
        SetupTRB.parameter = 0x01000680; // Low 4 bytes (Little Endian: 80 06 00 01)
        SetupTRB.status    = 0x00120000; // High 4 bytes (Little Endian: 00 00 12 00)
        
        SetupTRB.control   = (2u << 10)  // TRB Type 2 = Setup Stage
                | (2u << 16)  // TRT = 2 (IN Data Stage)
                | (0u << 6)   // IDT = 0 (use separate Data TRB)
                | (DRV.Devs[SlotID].EP0CycleState ? 1u : 0u);
        
        PushEP0TRB(DRV, SlotID, SetupTRB);

        xHCITRB DataTRB;
        DataTRB.parameter = DestBuffer->PhysAddr; // Alamat fisik buffer
        DataTRB.status    = 18;                   // Length transfer (18 bytes)
        
        DataTRB.control   = (3u << 10)  // TRB Type 3 = Data Stage
                | (1u << 16)  // DIR = 1 (IN - Device to Host)
                | (1u << 2)   // ENT (Evaluate Next TRB) - optional tapi bagus
                | (DRV.Devs[SlotID].EP0CycleState ? 1u : 0u);
        xHCITRB StatusTRB;
        StatusTRB.parameter = 0;
        StatusTRB.status    = 0;
        
        StatusTRB.control   = (4u << 10)  // TRB Type 4 = Status Stage
                            | (0u << 16)  // DIR = 0 (OUT - handshake dr Host)
                            | (1u << 5)   // IOC (Interrupt On Completion) - KABARI GW KALO KELAR!
                            | (DRV.Devs[SlotID].EP0CycleState ? 1u : 0u);

                            
        // Push Data TRB then Status TRB
        PushEP0TRB(DRV, SlotID, DataTRB);
        PushEP0TRB(DRV, SlotID, StatusTRB);

        // Before ringing the doorbell, dump EP0 ring contents for debug
        {
            PageAlloc::DMAAlloc::DMABuffer *r = DRV.Devs[SlotID].EP0Ring;
            if (r) {
                xHCITRB *table = (xHCITRB*)r->VirtAddr;
                U32 entries = (U32)(r->Size / sizeof(xHCITRB));
                Printk::Write(Printk::Level::LOG_DEBUG, " EP0Ring dump slot=%u entries=%u enq_idx=%u cycle=%u phys=0x%016llx\n",
                    (unsigned)SlotID, (unsigned)entries, (unsigned)DRV.Devs[SlotID].EP0EnqueueIdx, (unsigned)DRV.Devs[SlotID].EP0CycleState, (unsigned long long)r->PhysAddr);
                U32 dumpCount = DRV.Devs[SlotID].EP0EnqueueIdx + 2;
                if (dumpCount > entries) dumpCount = entries;
                for (U32 i = 0; i < dumpCount; ++i) {
                    xHCITRB *t = &table[i];
                    Write(Printk::Level::LOG_DEBUG, "  TRB[%u] param=0x%016llx status=0x%08x ctl=0x%08x\n",
                        (unsigned)i, (unsigned long long)t->parameter, (unsigned)t->status, (unsigned)t->control);
                }
                // Flush the ring again to make sure recent pushes reached memory
                for (UPTR p = (UPTR)r->VirtAddr; p < (UPTR)r->VirtAddr + r->Size; p += 64) {
                    asm volatile("clflush (%0)" :: "r"((void*)p) : "memory");
                }
                asm volatile ("mfence" ::: "memory");
            }
        }

        // ---------------------------------------------------------
        // RING DOORBELL!
        // ---------------------------------------------------------
        // Target: DB[SlotID]
        // Value: DCI (Device Context Index).
        // DCI untuk Endpoint 0 Control Bidirectional selalu = 1.
        DRV.doorbell_regs[SlotID] = 1;
        Printk::Write(Printk::Level::LOG_INFO, " GetDeviceDescriptor: doorbell rung slot=%u\n", (unsigned)SlotID);
    }

    VOID SetDeviceConfiguration(xHCIDriver &DRV, U8 SlotID, U8 ConfigValue){
        Printk::Write(Printk::Level::LOG_INFO, " xHCI: Setting Configuration %u for Slot %u\n", (unsigned)ConfigValue, (unsigned)SlotID);

        // Format Setup Packet: SET_CONFIGURATION
        // bmRequestType = 0x00 (Host to Device, Standard, Device)
        // bRequest      = 0x09 (SET_CONFIGURATION)
        // wValue        = ConfigValue
        // wIndex        = 0
        // wLength       = 0
        
        // Pack ke Parameter TRB (Low 4 bytes)
        // 0x00000900 | (ConfigValue << 16)
        U32 paramLow = 0x00000900 | (ConfigValue << 16);

        // TRB 1: SETUP STAGE
        xHCITRB SetupTRB;
        SetupTRB.parameter = paramLow; 
        SetupTRB.status    = 0; // High 4 bytes (wIndex=0, wLength=0)
        
        // TRT=2 (No Data Stage), IDT=1 (Immediate Data), Type=2 (Setup)
        SetupTRB.control   = (2u << 10) | (2u << 16) | (1u << 6) | 
                             (DRV.Devs[SlotID].EP0CycleState ? 1u : 0u);
        
        PushEP0TRB(DRV, SlotID, SetupTRB);

        // TRB 2: STATUS STAGE (IN Direction)
        xHCITRB StatusTRB;
        StatusTRB.parameter = 0;
        StatusTRB.status    = 0;
        // DIR=1 (IN), IOC=1 (Interrupt), Type=4 (Status)
        StatusTRB.control   = (4u << 10) | (1u << 16) | (1u << 5) |
                              (DRV.Devs[SlotID].EP0CycleState ? 1u : 0u);

        PushEP0TRB(DRV, SlotID, StatusTRB);

        // Ring Doorbell
        DRV.doorbell_regs[SlotID] = 1;
    }
}