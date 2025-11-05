#include <rosval.h>
#include <rossys.hpp>
#include <logging.hpp>
#include "string.hpp"
#include "xhci.hpp"
#include <drivers/pci.hpp>
#include <mm.hpp>
#include "../ahci/ahci_internal.hpp"
#include "xhci_regs.hpp"

namespace xHCI{
    using namespace Printk;
    xHCIDriver g_xhci_controllers[XHCI_MAX_CONTROLLERS];
    int g_xhci_controller_count = 0;

    // Forward declarations for local helpers
    static VOID SendEnableSlotCommand(xHCIDriver &DRV);
    static void DumpXHCIState(xHCIDriver &DRV, const char* tag);
    static VOID ProcessPendingEvents(xHCIDriver &DRV, U32 Controller_ID);

    static VOID ProcessPendingEvents(xHCIDriver &DRV, U32 Controller_ID){
        volatile xHCIInterrupterRegs *IR0 = &DRV.rt_regs->interrupter_regs[0];
        while(TRUE){
            U32 index = DRV.EventRingDequeueIndex;
            volatile xHCITRB *Event = &DRV.VEventRing[index];
            U32 control = Event->control;

            if ((control & 1u) != (DRV.EventRingCycleState ? 1u : 0u)) {
                break;
            }

            U8 EventType = (U8)((control >> 10) & 0x3Fu);

            // xHCI TRB Event Types (subset):
            // 32 = Transfer Event
            // 33 = Command Completion Event
            // 34 = Port Status Change Event
            if(EventType == 33){ // Command Completion Event
                U64 cmd_ptr = Event->parameter;
                U8  ccode   = (U8)(Event->status >> 24);
                U8  slotId  = (U8)(Event->control >> 24);
                Write(Level::LOG_INFO, "[XHCI] Controller %u - CCE: cc=%u slot=%u cmd_ptr=0x%016llx status=0x%08x ctl=0x%08x\n",
                    (unsigned)Controller_ID,
                    (unsigned)ccode,
                    (unsigned)slotId,
                    (unsigned long long)cmd_ptr,
                    (unsigned)Event->status,
                    (unsigned)Event->control);
            } else if (EventType == 34) {
                // Port Status Change Event
                U64 param = Event->parameter;
                U8 portId = (U8)((param >> 24) & 0xFFu); // xHCI: Port ID in bits [31:24]
                if (portId == 0 || portId > g_xhci_controllers[Controller_ID].PortCount) {
                    Write(Level::LOG_WARNING, "[XHCI] Controller %u - PSC with invalid PortID=%u (param=0x%016llx)\n",
                          (unsigned)Controller_ID, (unsigned)portId, (unsigned long long)param);
                } else {
                    U32 idx = (U32)portId - 1; // Port registers are 0-based, PortID is 1-based
                    volatile xHCIPortRegs* PR = &g_xhci_controllers[Controller_ID].port_regs[idx];
                    U32 portsc = PR->port_sc;
                    Write(Level::LOG_INFO, "[XHCI] Controller %u - PSC: Port%u PORTSC=0x%08x\n",
                          (unsigned)Controller_ID, (unsigned)portId, (unsigned)portsc);

                    // Clear Connect Status Change (CSC, bit 17) if set
                    if (portsc & (1u << 17)) {
                        PR->port_sc = (1u << 17);
                    }

                    // If device connected (CCS bit0) and port enabled (PED bit1), kick Enable Slot if not yet sent
                    if ((portsc & 0x1u) && (portsc & (1u << 1))) {
                        xHCIDriver &DRV2 = g_xhci_controllers[Controller_ID];
                        if (!DRV2.SentEnableSlot) {
                            Write(Level::LOG_INFO, "[XHCI] Controller %u - Port%u connected+enabled, issuing Enable Slot\n",
                                  (unsigned)Controller_ID, (unsigned)portId);
                            SendEnableSlotCommand(DRV2);
                            DRV2.SentEnableSlot = TRUE;
                                // Non-intrusive diagnostics: observe status without draining events
                                Arch::Time::Sleep(1);
                                DumpXHCIState(DRV2, "post-test-1ms");
                        }
                    }
                }
            } else {
                Write(Level::LOG_INFO, "[XHCI] Controller %u - Event Type %u (param=0x%016llx status=0x%08x ctl=0x%08x)\n",
                    (unsigned)Controller_ID, (unsigned)EventType,
                    (unsigned long long)Event->parameter,
                    (unsigned)Event->status,
                    (unsigned)Event->control);
            }

            index++;
            if(index == DRV.EventRingSize){
                index = 0;
                DRV.EventRingCycleState = !DRV.EventRingCycleState;
            }

            DRV.EventRingDequeueIndex = index;
        }

        // Ack Event Interrupt (EINT)
        DRV.op_regs->usb_sts = (1u << 3);
        U64 newDequeuePhys = DRV.DMA_EventRing->PhysAddr + ((U64)DRV.EventRingDequeueIndex * sizeof(xHCITRB));
        IR0->erdp = newDequeuePhys | (1u << 3);
        // Clear Interrupter Pending by writing '1' to IP (bit0) and ensure IE (bit1) stays set
        IR0->iman |= 1u;            // clear IP
        IR0->iman |= (1u << 1);     // keep IE enabled
    }

    static VOID xHCI_HandleInterrupt(VAL32 Controller_ID){
        Write(Level::LOG_CRIT, "[XHCI] Interrupt received from controller %u \n", (unsigned)Controller_ID);

        xHCIDriver &DRV = g_xhci_controllers[Controller_ID];

        U32 Status = DRV.op_regs->usb_sts;
        // Check Event Interrupt (EINT) bit (bit 3). Using (1<<3) not (1&3).
        if(!(Status & (1u << 3))){
            Write(Level::LOG_WARNING, "[XHCI] Spurious interrupt on controller %u\n", (unsigned)Controller_ID);
            return;
        }

        ProcessPendingEvents(DRV, Controller_ID);
    }

    // (Stub ISR, mirip AHCI)
    static void xHCI_InterruptHandler_C0() {
        xHCI_HandleInterrupt(0);
    }
    static void (*g_xhci_handlers[4])() = { xHCI_InterruptHandler_C0, nullptr, nullptr, nullptr };

    VOID RegisterController(U8 Bus, U8 Device, U8 Function, U8 MSICapOffset){
        if(g_xhci_controller_count >= XHCI_MAX_CONTROLLERS) {
            Write(Level::LOG_ERR, "[XHCI] Controller full\n");
        }

        // Handle potential 64-bit BAR for xHCI (commonly a 64-bit MMIO BAR)
        U32 Bar0LOW = PCI::ReadDword(Bus, Device, Function, 0x10);
        U32 Bar0HI  = PCI::ReadDword(Bus, Device, Function, 0x14);
        U64 BarType = (Bar0LOW & 0x6) >> 1; // 0=32-bit, 2=64-bit
        U64 RegPhys64 = (BarType == 0x2)
            ? (((U64)Bar0HI << 32) | (U64)(Bar0LOW & 0xFFFFFFF0))
            : (U64)(Bar0LOW & 0xFFFFFFF0);
        UPTR RegPhys = (UPTR)RegPhys64;
        // Align physical base down to page boundary before mapping
        UPTR RegPhysPage = RegPhys & ~(PAGE_SIZE - 1);

        // XHCI register space can span multiple pages (capability, op, runtime,
        // doorbells). Map a larger window (e.g. 16 pages = 64KiB) to cover
        // typical controllers instead of a single page which caused PFs.
        const SIZE_T MapPagesCount = 16;
        VOID *VirtAddr = PageAlloc::VirtualAllocPages(MapPagesCount);
        if(!VirtAddr){
            Write(Level::LOG_ERR, "[XHCI] Failed allocating virtual for XHCI\n");
            return;
        }

        // Avoid setting NX here (EFER.NXE may be clear during early boot).
        PFLAGS Flags = PAGE_PRESENT | PAGE_RW | PAGE_PCD;
        if(!PageAlloc::MapPages(KernelPML4, RegPhysPage, (UPTR)VirtAddr, MapPagesCount, Flags)) {
            Write(Level::LOG_ERR, "[XHCI] Failed mapping XHCI registers\n");
            return;
        }

        UPTR PageOffset = RegPhys & (PAGE_SIZE - 1);

        xHCIDriver &DRV = g_xhci_controllers[g_xhci_controller_count];
        DRV.bus = Bus;
        DRV.dev = Device;
        DRV.func = Function;
        DRV.regs_base = (volatile U8*)VirtAddr + PageOffset;
        DRV.Initialized = FALSE;
        DRV.IntVector = 0;

        if(MSICapOffset != 0){
            U8 Vector = MSI::EnableMSI(Bus, Device, Function, MSICapOffset, g_xhci_handlers[0]);
            if(Vector != 0){
                DRV.IntVector = Vector;
                Write(Level::LOG_INFO, "[XHCI] Enabled MSI on XHCI Controller %02X:%02X:%02X with vector 0x%02x\n",
                    (unsigned)Bus, (unsigned)Device, (unsigned)Function, (unsigned)Vector);
            } else {
                Write(Level::LOG_ERR, "[XHCI] Failed to enable MSI on XHCI Controller %02X:%02X:%02X\n",
                    (unsigned)Bus, (unsigned)Device, (unsigned)Function);
            }
        }

        g_xhci_controller_count++;

        Write(Level::LOG_INFO, "[XHCI] Registered XHCI Controller %02X:%02X:%02X\n",
            (unsigned)Bus, (unsigned)Device, (unsigned)Function);

    }

    static BOOL TakeOwnershipFromBIOS(xHCIDriver &DRV, volatile xHCIExtCapUSBLegSup *USBLegSupCap){
        Printk::Write(Printk::LOG_INFO, "[XHCI] Attempting to take ownership from BIOS...\n");

        USBLegSupCap->leg_sup_sem |= (1 << 24); // Set OS Owned Semaphore

        for(int i = 0 ; i < 5000; i++){
            if((USBLegSupCap->leg_sup_sem & (1 << 16)) == 0){
                Write(Printk::LOG_NOTICE, "[XHCI] Successfully took ownership from BIOS\n");
                return TRUE;
            }
            Arch::Time::Sleep(1); // 1 ms
        }

        Write(Printk::LOG_ERR, "[XHCI] Failed to take ownership from BIOS (timeout)\n");
        return FALSE;
    }

    static VOID SendNOOPCommand(xHCIDriver &DRV){
        Write(Level::LOG_NOTICE, "[XHCI] Submitting NOOP Command\n");

        U32 index = DRV.CmdRingEnqueueIndex; 
        volatile xHCITRB *NOOPTRB = &DRV.VCmdRing[index];

        NOOPTRB->parameter = 0;
        NOOPTRB->status = 0;
        // TRB Type 23 = No-Op Command. Set IOC (bit5) and the current cycle bit.
        NOOPTRB->control = (23u << 10) | (1u << 5) | (DRV.CmdRingCycleState ? 1u : 0u);

        // Advance enqueue pointer, respecting the link TRB reserved at the tail.
        index++;
        if(index == DRV.CmdRingSize - 1){
            index = 0;
            DRV.CmdRingCycleState = !DRV.CmdRingCycleState;
            volatile xHCITRB *LinkTRB = &DRV.VCmdRing[DRV.CmdRingSize - 1];
            LinkTRB->control = (6u << 10) | (1u << 1) | (DRV.CmdRingCycleState ? 1u : 0u);
        }
        DRV.CmdRingEnqueueIndex = index;

        // Ensure TRB writes visible before ringing doorbell
        asm volatile ("mfence" ::: "memory");
        DRV.doorbell_regs[0] = 0;

        Write(Level::LOG_NOTICE, "[XHCI] NOOP Command doorbelled\n");
    }

    static VOID SendEnableSlotCommand(xHCIDriver &DRV){
        Write(Level::LOG_NOTICE, "[XHCI] Submitting Enable Slot Command\n");

        U32 index = DRV.CmdRingEnqueueIndex;
        volatile xHCITRB *TRB = &DRV.VCmdRing[index];

        TRB->parameter = 0; // no params
        TRB->status = 0;
        // TRB Type 9 = Enable Slot Command. Set IOC and current cycle.
        TRB->control = (9u << 10) | (1u << 5) | (DRV.CmdRingCycleState ? 1u : 0u);

        U64 trb_phys = (U64)DRV.DMA_CmdRing->PhysAddr + ((U64)index * sizeof(xHCITRB));
        Write(Level::LOG_INFO, "[XHCI] Enable Slot TRB @ phys=0x%016llx idx=%u ctl=0x%08x\n",
              (unsigned long long)trb_phys, (unsigned)index, (unsigned)TRB->control);

        index++;
        if(index == DRV.CmdRingSize - 1){
            index = 0;
            DRV.CmdRingCycleState = !DRV.CmdRingCycleState;
            volatile xHCITRB *LinkTRB = &DRV.VCmdRing[DRV.CmdRingSize - 1];
            LinkTRB->control = (6u << 10) | (1u << 1) | (DRV.CmdRingCycleState ? 1u : 0u);
        }
        DRV.CmdRingEnqueueIndex = index;

        asm volatile ("mfence" ::: "memory");
        DRV.doorbell_regs[0] = 0;

        Write(Level::LOG_NOTICE, "[XHCI] Enable Slot doorbelled\n");
    }

    static BOOL PollEventRingForMs(xHCIDriver &DRV, U32 ms, U8 *outType){
        // Poll-based event processing removed; rely on MSI-driven ISR only.
        return FALSE;
    }

    static void DumpXHCIState(xHCIDriver &DRV, const char* tag) {
        volatile xHCIOpRegisters *op = DRV.op_regs;
        volatile xHCIRuntimeRegisters *rt = DRV.rt_regs;
        volatile xHCIInterrupterRegs *IR0 = &rt->interrupter_regs[0];

        Write(Level::LOG_INFO, "[XHCI] Dump (%s): usb_cmd=0x%08x usb_sts=0x%08x crcr=0x%016llx dcbaap=0x%016llx\n",
            tag, (unsigned)op->usb_cmd, (unsigned)op->usb_sts, (unsigned long long)op->crcr, (unsigned long long)op->dcbaap);

        U64 erdp_raw = IR0->erdp;
        U64 erdp_ptr = erdp_raw & ~0xFULL; // lower nibble holds EHB (bit3) and reserved bits
        U8  erdp_ehb = (U8)((erdp_raw >> 3) & 1u);
        Write(Level::LOG_INFO, "[XHCI] Dump (%s): IMAN=0x%08x IMOD=0x%08x ERSTSZ=%u ERSTBA=0x%016llx ERDP=0x%016llx (ptr=0x%016llx EHB=%u)\n",
            tag, (unsigned)IR0->iman, (unsigned)IR0->imod, (unsigned)IR0->erstsz,
            (unsigned long long)IR0->erstba, (unsigned long long)erdp_raw,
            (unsigned long long)erdp_ptr, (unsigned)erdp_ehb);

        // ERST table entry
        if (DRV.DMA_ERSTable) {
            volatile xHCIEventRingSegmentTableEntry *e = (volatile xHCIEventRingSegmentTableEntry*)DRV.DMA_ERSTable->VirtAddr;
            Write(Level::LOG_INFO, "[XHCI] Dump (%s): ERST[0].base=0x%016llx size=%u\n",
                tag, (unsigned long long)e->ring_segment_base_addr, (unsigned)e->ring_segment_size);
        }

        // EventRing snapshot
        if (DRV.VEventRing && DRV.EventRingSize) {
            U32 idx = DRV.EventRingDequeueIndex;
            unsigned long long param = (unsigned long long)DRV.VEventRing[idx].parameter;
            unsigned status = (unsigned)DRV.VEventRing[idx].status;
            unsigned control = (unsigned)DRV.VEventRing[idx].control;
            Write(Level::LOG_INFO, "[XHCI] Dump (%s): EventRing[%u] param=0x%016llx status=0x%08x control=0x%08x\n",
                tag, (unsigned)idx, param, status, control);
        }

        // Command Ring CRCR and doorbell (decode key bits: RCS, CA, CRR, CS)
        U64 crcr = op->crcr;
        U8 rcs = (U8)((crcr >> 0) & 1u);
        U8 ca  = (U8)((crcr >> 8) & 1u);   // Command Abort
        U8 crr = (U8)((crcr >> 9) & 1u);   // Command Ring Running
        U8 cs  = (U8)((crcr >> 10) & 1u);  // Command Stop
        Write(Level::LOG_INFO, "[XHCI] Dump (%s): CRCR=0x%016llx [RCS=%u CA=%u CRR=%u CS=%u] doorbell0=0x%08x\n",
            tag, (unsigned long long)crcr, (unsigned)rcs, (unsigned)ca, (unsigned)crr, (unsigned)cs, (unsigned)DRV.doorbell_regs[0]);
    }

    VOID InitializeAllControllers(){
        Write(Level::LOG_NOTICE, "[XHCI] Initializing all XHCI controllers (%d found)\n", g_xhci_controller_count);
        for(VAL32 i = 0; i < g_xhci_controller_count; i++){
            xHCIDriver &DRV = g_xhci_controllers[i];

            // Parse alamat register
            DRV.cap_regs = (volatile xHCICapRegisters*)DRV.regs_base;

            DRV.op_regs = (volatile xHCIOpRegisters*)(DRV.regs_base + DRV.cap_regs->cap_len);

            DRV.doorbell_regs = (volatile U32*)((UPTR)DRV.regs_base + DRV.cap_regs->dboff);

            Write(Level::LOG_INFO, "[XHCI] Controller %d - Version: %04x\n", (unsigned)i, (unsigned)DRV.cap_regs->hci_version);
            Write(Level::LOG_INFO, "[XHCI] Controller %d - HCS1=0x%08x HCS2=0x%08x\n",
                (unsigned)i, (unsigned)DRV.cap_regs->hcs_params1, (unsigned)DRV.cap_regs->hcs_params2);
            Write(Level::LOG_INFO, "[XHCI] Controller %d - Number of Slots: %u\n", (unsigned)i, (unsigned)(DRV.cap_regs->hcs_params1 & 0xFF));
            Write(Level::LOG_INFO, "[XHCI] OpRegs at %p, Doorbells at %p\n",
                (void*)DRV.op_regs, (void*)DRV.doorbell_regs);

            DRV.PortCount = (U8)((DRV.cap_regs->hcs_params1 >> 24) & 0xFF);
            DRV.port_regs = (volatile xHCIPortRegs*)((UPTR)DRV.op_regs + 0x400);
            DRV.SentEnableSlot = FALSE;
            Write(Level::LOG_INFO, "[XHCI] Controller %d - PortCount: %u\n", (unsigned)i, (unsigned)DRV.PortCount);

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

            Write(Level::LOG_INFO, "[XHCI] Resetting XHCI Controller %d...\n", (unsigned)i);

            volatile xHCIOpRegisters *op = DRV.op_regs;

            if((op->usb_cmd & 1) == 1){
                Write(Level::LOG_INFO, "[XHCI] Controller %d - stopping prior to reset: usb_cmd=0x%08x usb_sts=0x%08x\n",
                    (unsigned)i, (unsigned)op->usb_cmd, (unsigned)op->usb_sts);
                op->usb_cmd &= ~1;
                while((op->usb_sts & 1) == 1){
                    Arch::ASM::CPURelax();
                }
                Write(Level::LOG_INFO, "[XHCI] Controller %d stopped: usb_cmd=0x%08x usb_sts=0x%08x\n",
                    (unsigned)i, (unsigned)op->usb_cmd, (unsigned)op->usb_sts);
            }

            Write(Level::LOG_INFO, "[XHCI] Controller %d - initiating HCRST: usb_cmd=0x%08x usb_sts=0x%08x\n",
                (unsigned)i, (unsigned)op->usb_cmd, (unsigned)op->usb_sts);
            op->usb_cmd |= (1 << 1); // Set HCRST

            for(VAL32 t = 0; t < 5000; t++){
                if((op->usb_cmd & (1 << 1)) == 0){
                    Write(Level::LOG_INFO, "[XHCI] Controller %d reset complete: usb_cmd=0x%08x usb_sts=0x%08x\n",
                        (unsigned)i, (unsigned)op->usb_cmd, (unsigned)op->usb_sts);
                    break;
                }
                Arch::Time::Sleep(1);
            }

            if((op->usb_cmd & (1 << 1)) != 0){
                Write(Level::LOG_ERR, "[XHCI] Controller %d reset timeout\n", (unsigned)i);
                continue;
            }

            Write(Level::LOG_NOTICE, "[XHCI] Controller %d resetted succesfully\n", (unsigned)i);

            // Wait for Controller Not Ready (CNR, bit11 in USBSTS) to clear
            {
                const U32 CNR_MASK = (1u << 11);
                U32 waited_ms = 0;
                while ((op->usb_sts & CNR_MASK) != 0 && waited_ms < 500) {
                    Arch::Time::Sleep(1);
                    ++waited_ms;
                }
                if ((op->usb_sts & CNR_MASK) != 0) {
                    Write(Level::LOG_WARNING, "[XHCI] Controller %d - CNR still set after %u ms (usb_sts=0x%08x)\n",
                          (unsigned)i, (unsigned)waited_ms, (unsigned)op->usb_sts);
                } else {
                    Write(Level::LOG_INFO, "[XHCI] Controller %d - CNR cleared after %u ms (usb_sts=0x%08x)\n",
                          (unsigned)i, (unsigned)waited_ms, (unsigned)op->usb_sts);
                }
            }

            U8 MaxSlots = (DRV.cap_regs->hcs_params1 & 0xFF);
            DRV.op_regs->config = MaxSlots;
            Write(Level::LOG_INFO, "[XHCI] Controller %d - Configured for %u slots\n", (unsigned)i, (unsigned)MaxSlots);

            // Set system page size (bit0 = 4KiB). Required before programming DCBAAP/scratchpads.
            DRV.op_regs->page_size = 1u;
            Write(Level::LOG_INFO, "[XHCI] Controller %d - PAGESIZE set to 4KiB\n", (unsigned)i);

            DRV.DMA_DCBAAP = PageAlloc::DMAAlloc::AllocateDMAPages(1);
            if(!DRV.DMA_DCBAAP){
                Write(Level::LOG_ERR, "[XHCI] Controller %d - Failed to allocate DCBAAP\n", (unsigned)i);
                continue;
            }

            DRV.V_DCBAAP = (volatile U64*)DRV.DMA_DCBAAP->VirtAddr;
            String::Memset((void*)DRV.V_DCBAAP, 0, DRV.DMA_DCBAAP->Size);

            // Allocate scratchpad buffers if required by HCSPARAMS2 (Max Scratchpad Buffers)
            DRV.ScratchpadCount = 0;
            {
                U32 hcs2 = DRV.cap_regs->hcs_params2;
                U32 max_sp = (((hcs2 >> 27) & 0x1F) << 5) | ((hcs2 >> 21) & 0x1F);
                Write(Level::LOG_INFO, "[XHCI] Controller %d - Max Scratchpads (HCS2): %u\n", (unsigned)i, (unsigned)max_sp);
                if (max_sp > 64) max_sp = 64; // cap to 64 buffers for now
                if (max_sp > 0) {
                    // Allocate array of U64 phys pointers
                    SIZE_T arr_bytes = max_sp * sizeof(U64);
                    SIZE_T arr_pages = (arr_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
                    DRV.DMA_ScratchpadArray = PageAlloc::DMAAlloc::AllocateDMAPages(arr_pages);
                    if (!DRV.DMA_ScratchpadArray) {
                        Write(Level::LOG_ERR, "[XHCI] Controller %d - Failed to allocate Scratchpad Array\n", (unsigned)i);
                        // continue without, but commands may fail
                    } else {
                        volatile U64* arr = (volatile U64*)DRV.DMA_ScratchpadArray->VirtAddr;
                        String::Memset((void*)arr, 0, DRV.DMA_ScratchpadArray->Size);
                        // Allocate each scratchpad page and record phys
                        U32 ok_cnt = 0;
                        for (U32 s = 0; s < max_sp; ++s) {
                            DRV.DMA_Scratchpads[s] = PageAlloc::DMAAlloc::AllocateDMAPages(1);
                            if (!DRV.DMA_Scratchpads[s]) {
                                Write(Level::LOG_ERR, "[XHCI] Controller %d - Scratchpad %u alloc failed\n", (unsigned)i, (unsigned)s);
                                break;
                            }
                            arr[s] = (U64)DRV.DMA_Scratchpads[s]->PhysAddr;
                            ++ok_cnt;
                        }
                        DRV.ScratchpadCount = ok_cnt;
                        // Program DCBAA[0] to point to scratchpad array
                        DRV.V_DCBAAP[0] = (U64)DRV.DMA_ScratchpadArray->PhysAddr;
                        Write(Level::LOG_INFO, "[XHCI] Controller %d - Scratchpads: requested=%u allocated=%u array_phys=0x%llx\n",
                              (unsigned)i, (unsigned)max_sp, (unsigned)DRV.ScratchpadCount, (unsigned long long)DRV.DMA_ScratchpadArray->PhysAddr);
                    }
                }
            }

            // Some controllers don't accept a single 64-bit MMIO write reliably.
            // Split DCBAAP into two 32-bit writes (low then high) and issue a
            // memory barrier. Then read back to verify the value took effect.
            {
                U64 phys = (U64)DRV.DMA_DCBAAP->PhysAddr;
                volatile U32 *dcbaap_lo = (volatile U32*)((UPTR)DRV.op_regs + 0x30);
                volatile U32 *dcbaap_hi = (volatile U32*)((UPTR)DRV.op_regs + 0x34);
                U64 readback = 0;
                // Try writing hi then lo, up to 3 attempts, because some controllers
                // latch differently (writing low first can get overwritten).
                for (int attempt = 1; attempt <= 3; ++attempt) {
                    *dcbaap_hi = (U32)((phys >> 32) & 0xFFFFFFFFu);
                    *dcbaap_lo = (U32)(phys & 0xFFFFFFFFu);
                    asm volatile ("mfence" ::: "memory");
                    readback = DRV.op_regs->dcbaap;
                    Write(Level::LOG_INFO, "[XHCI] Controller %d - DCBAAP attempt %d wrote=0x%016llx readback=0x%016llx\n",
                        (unsigned)i, attempt, (unsigned long long)phys, (unsigned long long)readback);
                    if (readback == phys) break;
                    Arch::Time::Sleep(1);
                }
                if (readback != phys) {
                    Write(Level::LOG_WARNING, "[XHCI] Controller %d - DCBAAP did not stick after attempts\n", (unsigned)i);
                }
            }

            DRV.DMA_CmdRing = PageAlloc::DMAAlloc::AllocateDMAPages(1);
            if(!DRV.DMA_CmdRing){
                Write(Level::LOG_ERR, "[XHCI] Controller %d - Failed to allocate Command Ring\n", (unsigned)i);
                continue;
            }

            DRV.CmdRingSize = (U32)(DRV.DMA_CmdRing->Size / sizeof(xHCITRB));
            if(DRV.CmdRingSize < 2){
                Write(Level::LOG_ERR, "[XHCI] Controller %d - Command Ring too small (%u entries)\n",
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
                Write(Level::LOG_ERR, "[XHCI] Controller %d - Failed to allocate ERSTable\n", (unsigned)i);
                continue;
            }

            DRV.DMA_EventRing = PageAlloc::DMAAlloc::AllocateDMAPages(1);
            if(!DRV.DMA_EventRing){
                Write(Level::LOG_ERR, "[XHCI] Controller %d - Failed to allocate Event Ring\n", (unsigned)i);
                continue;
            }

            DRV.EventRingSize = (U32)(DRV.DMA_EventRing->Size / sizeof(xHCITRB));
            if(DRV.EventRingSize == 0){
                Write(Level::LOG_ERR, "[XHCI] Controller %d - Event Ring too small\n", (unsigned)i);
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
            Write(Level::LOG_INFO, "[XHCI] Controller %d - about to enable interrupts and start: usb_cmd=0x%08x usb_sts=0x%08x\n",
                (unsigned)i, (unsigned)DRV.op_regs->usb_cmd, (unsigned)DRV.op_regs->usb_sts);
            DRV.op_regs->usb_cmd |= (1u << 2); // INTE: Interrupt Enable
            DRV.op_regs->usb_cmd |= 1u;        // RS: Run/Stop = Run

            while(DRV.op_regs->usb_sts & 1){
                Arch::ASM::CPURelax();
            }

            Write(Level::LOG_NOTICE, "[XHCI] Controller %d running! usb_cmd=0x%08x usb_sts=0x%08x\n",
                (unsigned)i, (unsigned)DRV.op_regs->usb_cmd, (unsigned)DRV.op_regs->usb_sts);

            // Diagnostic dump before NOOP
            DumpXHCIState(DRV, "pre-noop");
            SendNOOPCommand(DRV);
            // Immediate dump after doorbell to capture state and possible pending IP
            DumpXHCIState(DRV, "post-noop");
            // Give controller a short moment and dump again to catch late updates (1ms)
            Arch::Time::Sleep(1);
            DumpXHCIState(DRV, "post-noop-1ms");

            // Enable Slot will now be issued upon Port Status Change (PSC) when a device connects

            DRV.Initialized = TRUE;
        }
    }

    // Issue an Enable Slot command for initialized controllers without using NOOP or polling.
    // Completion will be delivered via MSI as a Command Completion Event (CCE).
    VOID SimulateDeviceConnectTest(){
        Write(Level::LOG_NOTICE, "[XHCI] Test: Enable Slot without NOOP (interrupt-only)\n");
        for (VAL32 i = 0; i < g_xhci_controller_count; ++i) {
            xHCIDriver &DRV = g_xhci_controllers[i];
            if (!DRV.Initialized) {
                Write(Level::LOG_WARNING, "[XHCI] Controller %u not initialized, skipping test\n", (unsigned)i);
                continue;
            }
            if (!DRV.SentEnableSlot) {
                Write(Level::LOG_INFO, "[XHCI] Controller %u - issuing Enable Slot (no polling)\n", (unsigned)i);
                SendEnableSlotCommand(DRV);
                DRV.SentEnableSlot = TRUE;
                // Non-draining diagnostic: briefly wait and dump state to see if EINT is set and ERDP advanced.
                Arch::Time::Sleep(1);
                DumpXHCIState(DRV, "post-test-1ms");
                // If EINT is set but IMAN.IP is 0, briefly toggle IMAN.IE to re-arm MSI delivery (debug only, no event draining)
                {
                    volatile xHCIInterrupterRegs *IR0 = &DRV.rt_regs->interrupter_regs[0];
                    U32 usb_sts = DRV.op_regs->usb_sts;
                    U32 iman_before = IR0->iman;
                    if ((usb_sts & (1u << 3)) && ((iman_before & 1u) == 0)) {
                        // EINT pending, IP not set — re-arm IE edge
                        IR0->iman &= ~(1u << 1);
                        asm volatile ("mfence" ::: "memory");
                        IR0->iman |= (1u << 1);
                        U32 iman_after = IR0->iman;
                        Write(Level::LOG_INFO, "[XHCI] Controller %u - Re-armed IMAN.IE (iman: 0x%08x -> 0x%08x)\n",
                              (unsigned)i, (unsigned)iman_before, (unsigned)iman_after);
                    }
                }
                // Do not service EINT here; rely solely on MSI ISR to handle the event.
            } else {
                Write(Level::LOG_INFO, "[XHCI] Controller %u - Enable Slot already sent, skipping\n", (unsigned)i);
            }
        }
    }

    // Issue 'times' Enable Slot commands per initialized controller, without NOOP or polling.
    // This verifies that multiple MSIs are delivered and processed correctly.
    VOID InterruptBurstTest(U32 times) {
        if (times == 0) times = 1;
        Write(Level::LOG_NOTICE, "[XHCI] Test: Interrupt burst (Enable Slot x%u, no NOOP, no polling)\n", (unsigned)times);
        for (VAL32 i = 0; i < g_xhci_controller_count; ++i) {
            xHCIDriver &DRV = g_xhci_controllers[i];
            if (!DRV.Initialized) {
                Write(Level::LOG_WARNING, "[XHCI] Controller %u not initialized, skipping burst\n", (unsigned)i);
                continue;
            }
            for (U32 n = 0; n < times; ++n) {
                Write(Level::LOG_INFO, "[XHCI] Controller %u - burst %u/%u: issuing Enable Slot\n", (unsigned)i, (unsigned)(n+1), (unsigned)times);
                SendEnableSlotCommand(DRV);
                // Small spacing to avoid overwhelming some controllers; ISR handles events
                Arch::Time::Sleep(1);
            }
            // Post-burst diagnostic snapshot without draining events (ISR will handle them)
            DumpXHCIState(DRV, "post-burst");
        }
    }
}