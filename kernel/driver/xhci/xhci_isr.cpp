#define PRINTK_MODULE_NAME "XHCIIntr"
#include <rosval.h>
#include <rossys.hpp>
#include <logging.hpp>
#include <mm.hpp>
#include "string.hpp"
#include "xhci.hpp"
#include "xhci_regs.hpp"
#include "xhci_internal.hpp"
#include "../massusb/usb_defs.hpp"

namespace xHCI{
    using namespace Printk;

    // Map completed SlotID back to the root port that initiated the EnableSlot.
    // Strategy:
    // 1) Prefer ports that we marked as PORT_STATE_ENABLE_SENT when issuing
    //    the EnableSlot command (common path).
    // 2) Fallback: attempt to read the Slot Context RootPort from the DCBAAP
    //    entry if present.
    U8 GetPortIDForSlot(xHCIDriver &DRV, U8 SlotID) {
        // Search ports for the one in ENABLE_SENT state
        for (U32 p = 0; p < DRV.PortCount; ++p) {
            if (DRV.PortStates[p].State == xHCIDriver::PORT_STATE_ENABLE_SENT) {
                // Mark it as addressing to avoid reuse
                DRV.PortStates[p].State = xHCIDriver::PORT_STATE_ADDRESSING;
                return (U8)(p + 1); // Port IDs are 1-based
            }
        }

        // Fallback: try to read root port from the device context array
        if (DRV.V_DCBAAP) {
            U64 dcba_phys = (U64)DRV.V_DCBAAP[SlotID];
            if (dcba_phys) {
                // DCBAAP points to a device context base address (phys). Slot Context
                // is at offset 32 bytes into the device context structure.
                volatile U32 *slotCtx = (volatile U32*)HHDM_PhysToVirt((UPTR)dcba_phys + 32);
                // The RootPort is stored in SlotContext[1] bits 16..23 per earlier usage
                U32 slotCtx1 = slotCtx[1];
                U8 rootPort = (U8)((slotCtx1 >> 16) & 0xFF);
                if (rootPort != 0) {
                    Write(Level::LOG_DEBUG, " GetPortIDForSlot: derived rootPort %u from DCBAAP for slot %u\n", (unsigned)rootPort, (unsigned)SlotID);
                    return rootPort;
                }
            }
        }

        Write(Level::LOG_WARNING, " GetPortIDForSlot: Unable to find port for Slot %u; defaulting to 1\n", (unsigned)SlotID);
        return 0;
    }

    static VOID HandlePortStatusChange(xHCIDriver &DRV, U8 PortID){
        if(PortID == 0 || PortID > DRV.PortCount) {
            Write(Level::LOG_ERR, " Invalid PortID %u for Port Status Change\n", (unsigned)PortID);
            return;
        }

        VOLATILE xHCIPortRegs *PortReg = &DRV.port_regs[PortID - 1];
        U32 PortSC = PortReg->port_sc;

        Write(Level::LOG_INFO, " Port %u - PortSC=0x%08x\n", (unsigned)PortID, (unsigned)PortSC);

        // Cek bit CSC (Connect Status Change) - bit 17
        // Dan bit CCS (Current Connect Status) - bit 0
        if(PortSC & (1 << 17)){
            PortReg->port_sc = (1 << 17) | (1 << 9); // Clear CSC by writing 1

                if(PortSC & (1 << 0)){
                Write(Level::LOG_NOTICE, " xHCI: Device connected on Port %u resetting...\n", (unsigned)PortID);

                U32 ResetCMD = PortSC;
                ResetCMD &= ~(0xFE0000); // Clear bits 25-31
                ResetCMD |= (1 << 4);
                ResetCMD &= ~(1 << 1); // Clear PED to initiate reset

                PortReg->port_sc = ResetCMD;

                INTN Timeout = 100000;
                while(Timeout-- > 0){
                    if(PortReg->port_sc & (1 << 21)){
                        break; // Reset complete
                    }
                    Arch::ASM::CPURelax();
                }

                PortReg->port_sc |= (1 << 21);

                Write(Level::LOG_NOTICE, " xHCI: Port %u reset complete.\n", (unsigned)PortID);

                // Mark the port as having an EnableSlot pending so the
                // completion handler can correlate the returned SlotID
                // to this root port.
                if (PortID > 0 && PortID <= DRV.PortCount) {
                    DRV.PortStates[PortID - 1].State = xHCIDriver::PORT_STATE_ENABLE_SENT;
                }

                SendEnableSlotCommand(DRV);
            } else {
                Write(Level::LOG_NOTICE, " xHCI: Device disconnected from Port %u.\n", (unsigned)PortID);
            }
        }
    }

    static VOID ProcessPendingEvents(xHCIDriver &DRV, U32 Controller_ID){
        volatile xHCIInterrupterRegs *IR0 = &DRV.rt_regs->interrupter_regs[0];
        while(TRUE){
            U32 index = DRV.EventRingDequeueIndex;
            volatile xHCITRB *Event = &DRV.VEventRing[index];

            asm volatile("clflush (%0)" :: "r"(Event) : "memory");

            U32 control = Event->control;

            if ((control & 1u) != (DRV.EventRingCycleState ? 1u : 0u)) {
                break;
            }

            U8 EventType = (U8)((control >> 10) & 0x3Fu);
            Printk::Write(Printk::Level::LOG_DEBUG, " xHCI: Controller %u - Event Type %u detected (param=0x%016llx status=0x%08x ctl=0x%08x)\n",
                (unsigned)Controller_ID, (unsigned)EventType,
                (unsigned long long)Event->parameter,
                (unsigned)Event->status,
                (unsigned)Event->control);

            // 33 = Command Completion Event, 34 = Port Status Change Event
            if(EventType == 33){
                U8  ccode   = (U8)(Event->status >> 24);
                U8  slotId  = (U8)(Event->control >> 24);
                UNUSED__ U8 EndpointID = (U8)((Event->control >> 16) & 0x1F);

                //Write(Level::LOG_DEBUG, " Controller %u - CCE: cc=%u slot=%u cmd_ptr=0x%016llx status=0x%08x ctl=0x%08x\n",
                //    (unsigned)Controller_ID,
                //    (unsigned)ccode,
                //    (unsigned)slotId,
                //    (unsigned long long)cmd_ptr,
                //    (unsigned)Event->status,
                //    (unsigned)Event->control);

                if(ccode == 1){ // Success
                    U8 portWaitingEnable = 0;
                    for(U32 p=0; p<DRV.PortCount; p++){
                        if(DRV.PortStates[p].State == xHCIDriver::PORT_STATE_ENABLE_SENT){
                            portWaitingEnable = p + 1;
                            DRV.PortStates[p].State = xHCIDriver::PORT_STATE_ADDRESSING; 
                            break;
                        }
                    }

                    if (portWaitingEnable != 0) {
                        // Ini berarti command ENABLE SLOT baru selesai
                        Write(Level::LOG_INFO, " xHCI: Slot ID %u assigned to Port %u. Starting Address Device...\n", slotId, portWaitingEnable);
                        
                        // INI DIA YANG HILANG TADI:
                        SetupAddressDevice(DRV, slotId, portWaitingEnable); 

                    } else {
                        // Kalau gak ada port yang nunggu Enable Slot, berarti ini command lain.
                        // Asumsi flow linear: berarti ini ADDRESS DEVICE selesai.

                        Write(Level::LOG_INFO, " xHCI: Address Device Command Completed for Slot %u. Getting Descriptor...\n", slotId);

                        // Diagnostic dump: print the Event TRB contents we just processed
                        Write(Level::LOG_DEBUG, " CCE EventTRB: param=0x%016llx status=0x%08x ctl=0x%08x\n",
                            (unsigned long long)Event->parameter, (unsigned)Event->status, (unsigned)Event->control);

                        // Diagnostic: read device context base for this slot and dump EP0 dequeue pointer
                        if (DRV.V_DCBAAP) {
                            U64 dcba_phys = (U64)DRV.V_DCBAAP[slotId];
                            if (dcba_phys) {
                                volatile U32 *ep0ctx = (volatile U32*)HHDM_PhysToVirt((UPTR)dcba_phys + 64);
                                U32 dq_lo = ep0ctx[2];
                                U32 dq_hi = ep0ctx[3];
                                U64 dq_phys = ((U64)dq_hi << 32) | (U64)(dq_lo & 0xFFFFFFF0ULL);
                                U8 dcs = (U8)(dq_lo & 1u);
                                Write(Level::LOG_INFO, " Slot %u EP0 DequeuePtr lo=0x%08x hi=0x%08x DCS=%u -> phys=0x%016llx\n",
                                    (unsigned)slotId, (unsigned)dq_lo, (unsigned)dq_hi, (unsigned)dcs, (unsigned long long)dq_phys);
                            } else {
                                Write(Level::LOG_INFO, " No DCBAAP entry for slot %u (dcba_phys=0)\n", (unsigned)slotId);
                            }
                        } else {
                            Write(Level::LOG_INFO, " No V_DCBAAP pointer available for controller\n");
                        }

                        GetDeviceDescriptor(DRV, slotId);
                    }
                } else {
                        // Diagnostic: dump the command TRB referenced by this CCE (Event->parameter)
                        U64 cmd_ptr_phys = Event->parameter;
                        if (cmd_ptr_phys) {
                            volatile xHCITRB *cmd_trb = (volatile xHCITRB*)HHDM_PhysToVirt((UPTR)cmd_ptr_phys);
                            Write(Level::LOG_DEBUG, " CCE Error: cmd_trb at phys=0x%016llx param=0x%016llx status=0x%08x ctl=0x%08x\n",
                                (unsigned long long)cmd_ptr_phys, (unsigned long long)cmd_trb->parameter, (unsigned)cmd_trb->status, (unsigned)cmd_trb->control);
                            U8 trbType = (U8)((cmd_trb->control >> 10) & 0x3F);
                            Write(Level::LOG_DEBUG, " CCE Error: TRB Type=%u\n", (unsigned)trbType);
                            if (trbType == 11) { // Address Device command
                                U64 inctx_phys = cmd_trb->parameter;
                                if (inctx_phys) {
                                    volatile U32 *inp = (volatile U32*)HHDM_PhysToVirt((UPTR)inctx_phys);
                                    Write(Level::LOG_DEBUG, " AddressDevice InputContext (phys=0x%016llx):\n", (unsigned long long)inctx_phys);
                                    for (int i = 0; i < 8; ++i) {
                                        Write(Level::LOG_DEBUG, "  IC[%d]=0x%08x\n", i, (unsigned)inp[i]);
                                    }
                                }
                            }
                        } else {
                            Write(Level::LOG_DEBUG, " CCE Error: no cmd_trb pointer in Event->parameter\n");
                        }
                        Write(Level::LOG_WARNING, " xHCI: Command failed with completion code %u for Slot %u\n", (unsigned)ccode, (unsigned)slotId);
                }
            } else if (EventType == 32){
                U8 CCode = (U8)(Event->status >> 24);
                U8 SlotID = (U8)(Event->control >> 24);
                U8 EndpointID = (U8)((Event->control >> 16) & 0x1F);
                
                if(CCode == 1 && EndpointID == 1){
                    Write(Level::LOG_INFO, " xHCI: Get Device Descriptor command completed successfully for Slot %u\n", (unsigned)SlotID);

                    U64 DestPhys = DRV.Devs[SlotID].LastEP0DestPhys;
                    if(DestPhys){
                        USBDeviceDescriptor *Desc = (USBDeviceDescriptor*)(VOID*)HHDM_PhysToVirt((UPTR)DestPhys);

                        Write(Level::LOG_NOTICE, " DEVICE FOUND! VID: %04x PID: %04x Class: %02x\n", 
                            Desc->idVendor, Desc->idProduct, Desc->bDeviceClass);

                        if (Desc->bDeviceClass == 0x08 || Desc->bDeviceClass == 0x00) { 
                            // Class 0x00 berarti info class ada di Interface Descriptor (umum di flashdisk)
                            Write(Level::LOG_NOTICE, " !!! MASS STORAGE CANDIDATE !!!\n");
                            
                            // TRIGGER STEP 5: SET CONFIGURATION
                            SetDeviceConfiguration(DRV, SlotID, 1); // Config index 1 biasanya
                        }
                    }
                } else if (CCode != 1) {
                    Write(Level::LOG_ERR, " Transfer Failed Code %d Slot %d EP %d\n", CCode, SlotID, EndpointID);
                }
            } else if (EventType == 34) {
                U8 PortID = (U8)((Event->parameter >> 24) & 0xFF);

                Write(Level::LOG_NOTICE, " xHCI: Port Status Change detected on Port %d\n", PortID);

                HandlePortStatusChange(DRV, PortID);
            } else {
                Write(Level::LOG_INFO, " Controller %u - Event Type %u (param=0x%016llx status=0x%08x ctl=0x%08x)\n",
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
        // Clear Interrupter Pending (write-1-to-clear) and ensure IE stays enabled.
        // Use an assignment to write the proper bits (clear IP by writing 1, and set IE).
        // Some implementations expect writing 1 to IP clears it; OR-ing the read value
        // is incorrect because it may not write the required '1'. So write both bits.
        IR0->iman = (1u << 0) | (1u << 1);
    }

    static VOID xHCI_HandleInterrupt(VAL32 Controller_ID){
        Write(Level::LOG_INFO, " Interrupt received from controller %u \n", (unsigned)Controller_ID);
        xHCIDriver &DRV = g_xhci_controllers[Controller_ID];
        U32 Status = DRV.op_regs->usb_sts;
        if(!(Status & (1u << 3))){
            // Spurious interrupt: provide extra debug info to help diagnose IRQ races
            volatile xHCIInterrupterRegs *IR0 = &DRV.rt_regs->interrupter_regs[0];
            U32 iman = IR0->iman;
            Write(Level::LOG_INFO, " Spurious interrupt on controller %u\n", (unsigned)Controller_ID);
            Write(Level::LOG_INFO, " usb_sts=0x%08x IMAN=0x%08x ERDP=0x%016llx dequeue_idx=%u ring_size=%u\n",
                  (unsigned)Status,
                  (unsigned)iman,
                  (unsigned long long)IR0->erdp,
                  (unsigned)DRV.EventRingDequeueIndex,
                  (unsigned)DRV.EventRingSize);

            // If there are event TRBs present, dump the next couple for inspection
            if (DRV.VEventRing && DRV.EventRingSize) {
                U32 idx = DRV.EventRingDequeueIndex;
                for (int i = 0; i < 2; ++i) {
                    volatile xHCITRB *e = &DRV.VEventRing[idx];
                    Write(Level::LOG_INFO, " EventTRB[%u] param=0x%016llx status=0x%08x ctl=0x%08x\n",
                          (unsigned)idx,
                          (unsigned long long)e->parameter,
                          (unsigned)e->status,
                          (unsigned)e->control);
                    idx++;
                    if (idx == DRV.EventRingSize) idx = 0;
                }
            }

            // Increment spurious counter and, if no events are pending, attempt to clear
            // the interrupter pending bit to avoid IRQ storms.
            DRV.SpuriousInterruptCount++;

            // If IMAN.IP is set but Event Ring appears empty (next TRBs are zero), clear IP.
            // This is low-risk: it's an ack of an empty interrupt.
            if ( (iman & (1u << 1)) ) {
                bool ring_empty = true;
                if (DRV.VEventRing && DRV.EventRingSize) {
                    U32 idx2 = DRV.EventRingDequeueIndex;
                    // Check a few TRBs to see if non-zero
                    for (int i = 0; i < 4; ++i) {
                        volatile xHCITRB *et = &DRV.VEventRing[idx2];
                        if (et->control || et->status || et->parameter) { ring_empty = false; break; }
                        idx2++;
                        if (idx2 == DRV.EventRingSize) idx2 = 0;
                    }
                }

                if (ring_empty) {
                    // Re-arm the Event Ring Dequeue Pointer (ERDP) like the normal path does.
                    U64 newDequeuePhys = DRV.DMA_EventRing->PhysAddr + ((U64)DRV.EventRingDequeueIndex * sizeof(xHCITRB));
                    IR0->erdp = newDequeuePhys | (1u << 3);
                    asm volatile ("mfence" ::: "memory");

                    // Clear IP and ensure IE stays enabled (write 1 to IP, 1 to IE)
                    IR0->iman = (1u << 0) | (1u << 1);
                    U32 iman_after = IR0->iman;
                    Write(Level::LOG_INFO, " Re-armed ERDP and attempted clear IMAN.IP (before=0x%08x after=0x%08x)\n", (unsigned)iman, (unsigned)iman_after);

                    // If IMAN did not clear, try toggle IE fallback: disable IE then re-enable.
                    if (iman_after == iman) {
                        Write(Level::LOG_WARNING, " Controller %u - IMAN.IP did not clear, attempting IE toggle fallback (bus %u:%u:%u vector=0x%02x)\n",
                              (unsigned)Controller_ID, (unsigned)DRV.bus, (unsigned)DRV.dev, (unsigned)DRV.func, (unsigned)DRV.IntVector);
                        // Clear IE (write IMAN with IE=0, IP left alone)
                        IR0->iman = (0u << 0) | (0u << 1);
                        asm volatile ("mfence" ::: "memory");
                        // Re-enable IE and IP clear bit in case controller accepted it now
                        IR0->iman = (1u << 0) | (1u << 1);
                        asm volatile ("mfence" ::: "memory");
                        U32 iman_after2 = IR0->iman;
                        Write(Level::LOG_INFO, " IE toggle result IMAN before=0x%08x after_toggle=0x%08x\n", (unsigned)iman, (unsigned)iman_after2);
                    }
                }
            }

            // Warn if spurious interrupts accumulate
            const U32 SPURIOUS_WARN_THRESHOLD = 50;
            if (DRV.SpuriousInterruptCount && (DRV.SpuriousInterruptCount % SPURIOUS_WARN_THRESHOLD) == 0) {
                Write(Level::LOG_WARNING, " Controller %u - Spurious interrupts seen: %u\n", (unsigned)Controller_ID, (unsigned)DRV.SpuriousInterruptCount);
            }

            return;
        }
        ProcessPendingEvents(DRV, Controller_ID);
    }

    // ISR entry for controller 0 (accepts context parameter)
    void xHCI_InterruptHandler_C0(void *context) {
        (void)context;
        xHCI_HandleInterrupt(0);
    }
}
