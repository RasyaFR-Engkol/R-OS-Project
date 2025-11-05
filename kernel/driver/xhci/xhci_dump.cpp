#include <rosval.h>
#include <logging.hpp>
#include "xhci.hpp"
#include "xhci_regs.hpp"
#include "xhci_internal.hpp"

namespace xHCI {
    using namespace Printk;

    void DumpXHCIState(xHCIDriver &DRV, const char* tag) {
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
}
