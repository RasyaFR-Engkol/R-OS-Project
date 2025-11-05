#include <rosval.h>
#include <logging.hpp>
#include <mm.hpp>
#include "xhci.hpp"
#include "xhci_regs.hpp"
#include "xhci_internal.hpp"

namespace xHCI {
    using namespace Printk;

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
            // Issue per-port Enable Slot for any port that has not yet had an EnableSlot sent.
            for (U32 p = 0; p < DRV.PortCount; ++p) {
                if (DRV.PortStates[p].State != xHCIDriver::PORT_STATE_ENABLE_SENT) {
                    Write(Level::LOG_INFO, "[XHCI] Controller %u - issuing Enable Slot for Port %u (no polling)\n", (unsigned)i, (unsigned)(p+1));
                    SendEnableSlotCommand(DRV);
                    DRV.PortStates[p].State = xHCIDriver::PORT_STATE_ENABLE_SENT;
                    break; // issue one for now (original test issued one per controller)
                }
            }
                // Non-draining diagnostic: briefly wait and dump state to see if EINT is set and ERDP advanced.
                Arch::Time::Sleep(1);
                DumpXHCIState(DRV, "post-test-1ms");
                // If EINT is set but IMAN.IP is 0, briefly toggle IMAN.IE to re-arm MSI delivery (debug only, no event draining)
                {
                    volatile xHCIInterrupterRegs *IR0 = &DRV.rt_regs->interrupter_regs[0];
                    U32 usb_sts = DRV.op_regs->usb_sts;
                    U32 iman_before = IR0->iman;
                    if ((usb_sts & (1u << 3)) && ((iman_before & 1u) == 0)) {
                        IR0->iman &= ~(1u << 1);
                        asm volatile ("mfence" ::: "memory");
                        IR0->iman |= (1u << 1);
                        U32 iman_after = IR0->iman;
                        Write(Level::LOG_INFO, "[XHCI] Controller %u - Re-armed IMAN.IE (iman: 0x%08x -> 0x%08x)\n",
                              (unsigned)i, (unsigned)iman_before, (unsigned)iman_after);
                    }
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
                Arch::Time::Sleep(1);
            }
            DumpXHCIState(DRV, "post-burst");
        }
    }
}
