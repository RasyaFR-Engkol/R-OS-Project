#define PRINTK_MODULE_NAME "XHCIInit"
#include <rosval.h>
#include <rossys.hpp>
#include <logging.hpp>
#include <mm.hpp>
#include "string.hpp"
#include "xhci.hpp"
#include "xhci_regs.hpp"
#include "xhci_internal.hpp"

namespace xHCI{
    using namespace Printk;

    static BOOL TakeOwnershipFromBIOS(xHCIDriver &DRV, volatile xHCIExtCapUSBLegSup *USBLegSupCap){
        Write(Level::LOG_INFO, " Attempting to take ownership from BIOS...\n");

        USBLegSupCap->leg_sup_sem |= (1 << 24); // Set OS Owned Semaphore

        for(int i = 0 ; i < 5000; i++){
            if((USBLegSupCap->leg_sup_sem & (1 << 16)) == 0){
                Write(Level::LOG_NOTICE, " Successfully took ownership from BIOS\n");
                return TRUE;
            }
            Arch::Time::Sleep(1); // 1 ms
        }

        Write(Level::LOG_ERR, " Failed to take ownership from BIOS (timeout)\n");
        return FALSE;
    }

    VOID InitializeAllControllers(){
        Write(Level::LOG_NOTICE, " Initializing all XHCI controllers (%d found)\n", g_xhci_controller_count);
        for(VAL32 i = 0; i < g_xhci_controller_count; i++){
            xHCIDriver &DRV = g_xhci_controllers[i];

            // Parse alamat register
            DRV.cap_regs = (volatile xHCICapRegisters*)DRV.regs_base;

            DRV.op_regs = (volatile xHCIOpRegisters*)(DRV.regs_base + DRV.cap_regs->cap_len);

            DRV.doorbell_regs = (volatile U32*)((UPTR)DRV.regs_base + DRV.cap_regs->dboff);

            Write(Level::LOG_INFO, " Controller %d - Version: %04x\n", (unsigned)i, (unsigned)DRV.cap_regs->hci_version);
            Write(Level::LOG_INFO, " Controller %d - HCS1=0x%08x HCS2=0x%08x\n",
                (unsigned)i, (unsigned)DRV.cap_regs->hcs_params1, (unsigned)DRV.cap_regs->hcs_params2);
            Write(Level::LOG_INFO, " Controller %d - Number of Slots: %u\n", (unsigned)i, (unsigned)(DRV.cap_regs->hcs_params1 & 0xFF));
            Write(Level::LOG_INFO, " OpRegs at %p, Doorbells at %p\n",
                (void*)DRV.op_regs, (void*)DRV.doorbell_regs);

            DRV.PortCount = (U8)((DRV.cap_regs->hcs_params1 >> 24) & 0xFF);
            DRV.port_regs = (volatile xHCIPortRegs*)((UPTR)DRV.op_regs + 0x400);
            // Initialize per-port state array
            for (U32 __p = 0; __p < 256; ++__p) {
                DRV.PortStates[__p].State = xHCIDriver::PORT_STATE_EMPTY;
            }
            Write(Level::LOG_INFO, " Controller %d - PortCount: %u\n", (unsigned)i, (unsigned)DRV.PortCount);

            U32 xECP = (DRV.cap_regs->hcc_params1 >> 16);
            volatile U8 *PTR = DRV.regs_base + (xECP * 4);

            while(PTR != nullptr){
                U8 CapID = *PTR;
                if(CapID == 1){
                    if (TakeOwnershipFromBIOS(DRV, (volatile xHCIExtCapUSBLegSup*)PTR)) {
                        break; // Sukses!
                    }
                }

                U8 NextPTR = *(PTR + 1);
                if(NextPTR == 0) break;
                PTR += (NextPTR * 4);
            }

            Write(Level::LOG_INFO, " Resetting XHCI Controller %d...\n", (unsigned)i);

            volatile xHCIOpRegisters *op = DRV.op_regs;

            if((op->usb_cmd & 1) == 1){
                Write(Level::LOG_INFO, " Controller %d - stopping prior to reset: usb_cmd=0x%08x usb_sts=0x%08x\n",
                    (unsigned)i, (unsigned)op->usb_cmd, (unsigned)op->usb_sts);
                op->usb_cmd &= ~1;
                while((op->usb_sts & 1) == 1){
                    Arch::ASM::CPURelax();
                }
                Write(Level::LOG_INFO, " Controller %d stopped: usb_cmd=0x%08x usb_sts=0x%08x\n",
                    (unsigned)i, (unsigned)op->usb_cmd, (unsigned)op->usb_sts);
            }

            Write(Level::LOG_INFO, " Controller %d - initiating HCRST: usb_cmd=0x%08x usb_sts=0x%08x\n",
                (unsigned)i, (unsigned)op->usb_cmd, (unsigned)op->usb_sts);
            op->usb_cmd |= (1 << 1); // Set HCRST

            for(VAL32 t = 0; t < 5000; t++){
                if((op->usb_cmd & (1 << 1)) == 0){
                    Write(Level::LOG_INFO, " Controller %d reset complete: usb_cmd=0x%08x usb_sts=0x%08x\n",
                        (unsigned)i, (unsigned)op->usb_cmd, (unsigned)op->usb_sts);
                    break;
                }
                Arch::Time::Sleep(1);
            }

            if((op->usb_cmd & (1 << 1)) != 0){
                Write(Level::LOG_ERR, " Controller %d reset timeout\n", (unsigned)i);
                continue;
            }

            Write(Level::LOG_NOTICE, " Controller %d resetted succesfully\n", (unsigned)i);

            // Wait for Controller Not Ready (CNR, bit11 in USBSTS) to clear
            {
                const U32 CNR_MASK = (1u << 11);
                U32 waited_ms = 0;
                while ((op->usb_sts & CNR_MASK) != 0 && waited_ms < 500) {
                    Arch::Time::Sleep(1);
                    ++waited_ms;
                }
                if ((op->usb_sts & CNR_MASK) != 0) {
                    Write(Level::LOG_WARNING, " Controller %d - CNR still set after %u ms (usb_sts=0x%08x)\n",
                          (unsigned)i, (unsigned)waited_ms, (unsigned)op->usb_sts);
                } else {
                    Write(Level::LOG_INFO, " Controller %d - CNR cleared after %u ms (usb_sts=0x%08x)\n",
                          (unsigned)i, (unsigned)waited_ms, (unsigned)op->usb_sts);
                }
            }

            U8 MaxSlots = (DRV.cap_regs->hcs_params1 & 0xFF);
            DRV.op_regs->config = MaxSlots;
            Write(Level::LOG_INFO, " Controller %d - Configured for %u slots\n", (unsigned)i, (unsigned)MaxSlots);

            // Set system page size (bit0 = 4KiB). Required before programming DCBAAP/scratchpads.
            DRV.op_regs->page_size = 1u;
            Write(Level::LOG_INFO, " Controller %d - PAGESIZE set to 4KiB\n", (unsigned)i);

            DRV.DMA_DCBAAP = PageAlloc::DMAAlloc::AllocateDMAPages(1);
            if(!DRV.DMA_DCBAAP){
                Write(Level::LOG_ERR, " Controller %d - Failed to allocate DCBAAP\n", (unsigned)i);
                continue;
            }

            DRV.V_DCBAAP = (volatile U64*)DRV.DMA_DCBAAP->VirtAddr;
            String::Memset((void*)DRV.V_DCBAAP, 0, DRV.DMA_DCBAAP->Size);

            // Allocate scratchpad buffers if required by HCSPARAMS2 (Max Scratchpad Buffers)
            DRV.ScratchpadCount = 0;
            {
                U32 hcs2 = DRV.cap_regs->hcs_params2;
                U32 max_sp = (((hcs2 >> 27) & 0x1F) << 5) | ((hcs2 >> 21) & 0x1F);
                Write(Level::LOG_INFO, " Controller %d - Max Scratchpads (HCS2): %u\n", (unsigned)i, (unsigned)max_sp);
                if (max_sp > 64) max_sp = 64; // cap to 64 buffers for now
                if (max_sp > 0) {
                    // Allocate array of U64 phys pointers
                    SIZE_T arr_bytes = max_sp * sizeof(U64);
                    SIZE_T arr_pages = (arr_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
                    DRV.DMA_ScratchpadArray = PageAlloc::DMAAlloc::AllocateDMAPages(arr_pages);
                    if (!DRV.DMA_ScratchpadArray) {
                        Write(Level::LOG_ERR, " Controller %d - Failed to allocate Scratchpad Array\n", (unsigned)i);
                        // continue without, but commands may fail
                    } else {
                        volatile U64* arr = (volatile U64*)DRV.DMA_ScratchpadArray->VirtAddr;
                        String::Memset((void*)arr, 0, DRV.DMA_ScratchpadArray->Size);
                        // Allocate each scratchpad page and record phys
                        U32 ok_cnt = 0;
                        for (U32 s = 0; s < max_sp; ++s) {
                            DRV.DMA_Scratchpads[s] = PageAlloc::DMAAlloc::AllocateDMAPages(1);
                            if (!DRV.DMA_Scratchpads[s]) {
                                Write(Level::LOG_ERR, " Controller %d - Scratchpad %u alloc failed\n", (unsigned)i, (unsigned)s);
                                break;
                            }
                            arr[s] = (U64)DRV.DMA_Scratchpads[s]->PhysAddr;
                            ++ok_cnt;
                        }
                        DRV.ScratchpadCount = ok_cnt;
                        // Program DCBAA[0] to point to scratchpad array
                        DRV.V_DCBAAP[0] = (U64)DRV.DMA_ScratchpadArray->PhysAddr;
                        Write(Level::LOG_INFO, " Controller %d - Scratchpads: requested=%u allocated=%u array_phys=0x%llx\n",
                              (unsigned)i, (unsigned)max_sp, (unsigned)DRV.ScratchpadCount, (unsigned long long)DRV.DMA_ScratchpadArray->PhysAddr);
                    }
                }
            }

            // DCBAAP: split writes and verify
            {
                U64 phys = (U64)DRV.DMA_DCBAAP->PhysAddr;
                volatile U32 *dcbaap_lo = (volatile U32*)((UPTR)DRV.op_regs + 0x30);
                volatile U32 *dcbaap_hi = (volatile U32*)((UPTR)DRV.op_regs + 0x34);
                U64 readback = 0;
                for (int attempt = 1; attempt <= 3; ++attempt) {
                    *dcbaap_hi = (U32)((phys >> 32) & 0xFFFFFFFFu);
                    *dcbaap_lo = (U32)(phys & 0xFFFFFFFFu);
                    asm volatile ("mfence" ::: "memory");
                    readback = DRV.op_regs->dcbaap;
                    Write(Level::LOG_INFO, " Controller %d - DCBAAP attempt %d wrote=0x%016llx readback=0x%016llx\n",
                        (unsigned)i, attempt, (unsigned long long)phys, (unsigned long long)readback);
                    if (readback == phys) break;
                    Arch::Time::Sleep(1);
                }
                if (readback != phys) {
                    Write(Level::LOG_WARNING, " Controller %d - DCBAAP did not stick after attempts\n", (unsigned)i);
                }
            }

            DRV.DMA_CmdRing = PageAlloc::DMAAlloc::AllocateDMAPages(1);
            if(!DRV.DMA_CmdRing){
                Write(Level::LOG_ERR, " Controller %d - Failed to allocate Command Ring\n", (unsigned)i);
                continue;
            }

            DRV.CmdRingSize = (U32)(DRV.DMA_CmdRing->Size / sizeof(xHCITRB));
            if(DRV.CmdRingSize < 2){
                Write(Level::LOG_ERR, " Controller %d - Command Ring too small (%u entries)\n",
                    (unsigned)i, (unsigned)DRV.CmdRingSize);
                continue;
            }

            DRV.VCmdRing = (volatile xHCITRB*)DRV.DMA_CmdRing->VirtAddr;
            String::Memset((void*)DRV.VCmdRing, 0, DRV.DMA_CmdRing->Size);

            DRV.CmdRingEnqueueIndex = 0;
            DRV.CmdRingCycleState = TRUE;

            // Reserve the last TRB as a link back to the start so the command ring loops.
            U32 linkIndex = DRV.CmdRingSize - 1;
            volatile xHCITRB *LinkTRB = &DRV.VCmdRing[linkIndex];
            LinkTRB->parameter = DRV.DMA_CmdRing->PhysAddr;
            LinkTRB->status = 0;
            LinkTRB->control = (6u << 10) | (1u << 1) | (DRV.CmdRingCycleState ? 1u : 0u); // Link TRB, toggle cycle on wrap

            // Seed CRCR with the ring base and current cycle state so the HC sees our producer phase.
            DRV.op_regs->crcr = DRV.DMA_CmdRing->PhysAddr | (DRV.CmdRingCycleState ? 1ull : 0ull);

            DRV.DMA_ERSTable = PageAlloc::DMAAlloc::AllocateDMAPages(1);
            if(!DRV.DMA_ERSTable){
                Write(Level::LOG_ERR, " Controller %d - Failed to allocate ERSTable\n", (unsigned)i);
                continue;
            }

            DRV.DMA_EventRing = PageAlloc::DMAAlloc::AllocateDMAPages(1);
            if(!DRV.DMA_EventRing){
                Write(Level::LOG_ERR, " Controller %d - Failed to allocate Event Ring\n", (unsigned)i);
                continue;
            }

            DRV.EventRingSize = (U32)(DRV.DMA_EventRing->Size / sizeof(xHCITRB));
            if(DRV.EventRingSize == 0){
                Write(Level::LOG_ERR, " Controller %d - Event Ring too small\n", (unsigned)i);
                continue;
            }

            DRV.VEventRing = (volatile xHCITRB*)DRV.DMA_EventRing->VirtAddr;
            String::Memset((void*)DRV.VEventRing, 0, DRV.DMA_EventRing->Size);
            DRV.EventRingCycleState = TRUE;
            DRV.EventRingDequeueIndex = 0;

            volatile xHCIEventRingSegmentTableEntry *ERST_Entry =
                (volatile xHCIEventRingSegmentTableEntry*)DRV.DMA_ERSTable->VirtAddr;

            ERST_Entry->ring_segment_base_addr = DRV.DMA_EventRing->PhysAddr;
            ERST_Entry->ring_segment_size = DRV.EventRingSize;

            DRV.rt_regs = (volatile xHCIRuntimeRegisters*)(DRV.regs_base + DRV.cap_regs->rtsoff);

            volatile xHCIInterrupterRegs *IR0 = &DRV.rt_regs->interrupter_regs[0];

            // Program Interrupter 0: ERST size/base and dequeue pointer (set EHB)
            IR0->erstsz = 1; // 1 entry
            IR0->erstba = DRV.DMA_ERSTable->PhysAddr; // Set ERST Base Address
            // Set Dequeue Pointer and clear Event Handler Busy (EHB) to signal readiness
            IR0->erdp = DRV.DMA_EventRing->PhysAddr | (1u << 3);
            // Clear any pending and enable interrupter
            IR0->iman = (1u << 1) | 1u;

            // Enable host controller interrupts globally and run the HC
            Write(Level::LOG_INFO, " Controller %d - about to enable interrupts and start: usb_cmd=0x%08x usb_sts=0x%08x\n",
                (unsigned)i, (unsigned)DRV.op_regs->usb_cmd, (unsigned)DRV.op_regs->usb_sts);
            DRV.op_regs->usb_cmd |= (1u << 2); // INTE: Interrupt Enable
            DRV.op_regs->usb_cmd |= 1u;        // RS: Run/Stop = Run

            while(DRV.op_regs->usb_sts & 1){
                Arch::ASM::CPURelax();
            }

            Write(Level::LOG_NOTICE, " Controller %d running! usb_cmd=0x%08x usb_sts=0x%08x\n",
                (unsigned)i, (unsigned)DRV.op_regs->usb_cmd, (unsigned)DRV.op_regs->usb_sts);

            // Diagnostic dump before NOOP
            //DumpXHCIState(DRV, "pre-noop");
            SendNOOPCommand(DRV);
            // Immediate dump after doorbell to capture state and possible pending IP
            //DumpXHCIState(DRV, "post-noop");
            // Give controller a short moment and dump again to catch late updates (1ms)
            Arch::Time::Sleep(1);
            //DumpXHCIState(DRV, "post-noop-1ms");

            // Enable Slot will now be issued upon Port Status Change (PSC) when a device connects

            DRV.Initialized = TRUE;
        }
    }
}
