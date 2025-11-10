#define PRINTK_MODULE_NAME "XHCIIntr"
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

            // 33 = Command Completion Event, 34 = Port Status Change Event
            if(EventType == 33){
                U64 cmd_ptr = Event->parameter;
                U8  ccode   = (U8)(Event->status >> 24);
                U8  slotId  = (U8)(Event->control >> 24);
                Write(Level::LOG_INFO, " Controller %u - CCE: cc=%u slot=%u cmd_ptr=0x%016llx status=0x%08x ctl=0x%08x\n",
                    (unsigned)Controller_ID,
                    (unsigned)ccode,
                    (unsigned)slotId,
                    (unsigned long long)cmd_ptr,
                    (unsigned)Event->status,
                    (unsigned)Event->control);
            } else if (EventType == 34) {
                // Port Status Change: handle connect, reset, and enable sequencing
                U64 param = Event->parameter;
                U8 portId = (U8)((param >> 24) & 0xFFu);
                if (portId == 0 || portId > DRV.PortCount) {
                    Write(Level::LOG_WARNING, " Controller %u - PSC with invalid PortID=%u (param=0x%016llx)\n",
                          (unsigned)Controller_ID, (unsigned)portId, (unsigned long long)param);
                } else {
                    U32 idx = (U32)portId - 1;
                    volatile xHCIPortRegs* PR = &DRV.port_regs[idx];
                    /*
                     * Avoid taking the address of a packed member (compiler warns/error):
                     * read the port_sc value through the volatile struct pointer instead.
                     */
                    U32 portsc_val = PR->port_sc; // read current PORTSC value
                    Write(Level::LOG_INFO, " Controller %u - PSC: Port%u PORTSC=0x%08x\n",
                          (unsigned)Controller_ID, (unsigned)portId, (unsigned)portsc_val);

                    U32 NewVAL = portsc_val & ((1u << 9) | (1u << 16));
                    NewVAL |= (portsc_val & 0x00FE0000);

                    BOOL ccs = (portsc_val & (1u << 0)) != 0; // connected
                    BOOL ped = (portsc_val & (1u << 1)) != 0; // enabled

                    if (!ccs) {
                        // Device removed: mark port empty so future EnableSlot may be attempted
                        DRV.PortStates[idx].State = xHCIDriver::PORT_STATE_EMPTY;
                    } else if (ccs && !ped) {
                        // Connected but not enabled: initiate Port Reset first (do NOT sleep in ISR)
                Write(Level::LOG_INFO, " Controller %u - Port%u connected but disabled, issuing Port Reset\n",
                    (unsigned)Controller_ID, (unsigned)portId);
                // Extra debug: log raw PORTSC value when device is plugged in
                Write(Level::LOG_INFO, " Controller %u - Port%u PORTSC raw=0x%08x (ccs=%u ped=%u)\n",
                    (unsigned)Controller_ID, (unsigned)portId, (unsigned)portsc_val, (unsigned)ccs, (unsigned)ped);
                        // Mark port as resetting
                        DRV.PortStates[idx].State = xHCIDriver::PORT_STATE_RESETTING;
                        NewVAL |= (1u << 4); // Set PR (bit4) to initiate Port Reset
                        NewVAL |= (1u << 16); // Set PRC (bit16) change bit
                        // Wait for PRC/PED via subsequent PSC; we'll enable slot then
                    } else if (ccs && ped) {
                        // Port is connected and enabled: now it's valid to Enable Slot
                        if (DRV.PortStates[idx].State != xHCIDriver::PORT_STATE_ENABLE_SENT) {
                            Write(Level::LOG_INFO, " Controller %u - Port%u enabled, issuing Enable Slot\n",
                                  (unsigned)Controller_ID, (unsigned)portId);
                            // Extra debug: log raw PORTSC value when device is enabled
                            Write(Level::LOG_INFO, " Controller %u - Port%u PORTSC raw=0x%08x (ccs=%u ped=%u)\n",
                                  (unsigned)Controller_ID, (unsigned)portId, (unsigned)portsc_val, (unsigned)ccs, (unsigned)ped);
                            SendEnableSlotCommand(DRV);
                            DRV.PortStates[idx].State = xHCIDriver::PORT_STATE_ENABLE_SENT;
                        }
                    }

                    /* Write the modified PORTSC back to the port register to request changes
                     * (for example, to initiate Port Reset we set PR and PRC bits above).
                     */
                    PR->port_sc = NewVAL;
                }
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
        Write(Level::LOG_CRIT, " Interrupt received from controller %u \n", (unsigned)Controller_ID);
        xHCIDriver &DRV = g_xhci_controllers[Controller_ID];
        U32 Status = DRV.op_regs->usb_sts;
        if(!(Status & (1u << 3))){
            // Spurious interrupt: provide extra debug info to help diagnose IRQ races
            volatile xHCIInterrupterRegs *IR0 = &DRV.rt_regs->interrupter_regs[0];
            U32 iman = IR0->iman;
            Write(Level::LOG_WARNING, " Spurious interrupt on controller %u\n", (unsigned)Controller_ID);
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

    // ISR entry for controller 0
    void xHCI_InterruptHandler_C0() {
        xHCI_HandleInterrupt(0);
    }
}
