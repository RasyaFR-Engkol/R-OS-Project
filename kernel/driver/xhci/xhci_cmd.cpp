#define PRINTK_MODULE_NAME "XHCICMD"
#include <rosval.h>
#include <logging.hpp>
#include <mm.hpp>
#include "xhci.hpp"
#include "xhci_regs.hpp"
#include "xhci_internal.hpp"

namespace xHCI {
    using namespace Printk;

    VOID SendNOOPCommand(xHCIDriver &DRV){
        Write(Level::LOG_DEBUG, " Submitting NOOP Command\n");

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

        Write(Level::LOG_DEBUG, " NOOP Command doorbelled\n");
    }

    VOID SendEnableSlotCommand(xHCIDriver &DRV){
        Write(Level::LOG_NOTICE, " Submitting Enable Slot Command\n");

        U32 index = DRV.CmdRingEnqueueIndex;
        volatile xHCITRB *TRB = &DRV.VCmdRing[index];

        TRB->parameter = 0; // no params
        TRB->status = 0;
        // TRB Type 9 = Enable Slot Command. Set IOC and current cycle.
        TRB->control = (9u << 10) | (1u << 5) | (DRV.CmdRingCycleState ? 1u : 0u);

        U64 trb_phys = (U64)DRV.DMA_CmdRing->PhysAddr + ((U64)index * sizeof(xHCITRB));
        Write(Level::LOG_INFO, " Enable Slot TRB @ phys=0x%016llx idx=%u ctl=0x%08x\n",
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

        Write(Level::LOG_NOTICE, " Enable Slot doorbelled\n");
    }
}
