#define PRINTK_MODULE_NAME "AHCIIntr"
#include <rosval.h>
#include "ahci.hpp"
#include "ahci_regs.hpp"
#include "logging.hpp"
#include "ahci_internal.hpp"

/* module name provided via PRINTK_MODULE_NAME */

namespace AHCI {
    // ISR wrappers for controllers. Updated to accept void* context and
    // forward to the controller-specific handler. Keep them small.
    static void AHCI_InterruptHandler_C0(void *context) { AHCI::HandleInterrupt(0); }
    static void AHCI_InterruptHandler_C1(void *context) { AHCI::HandleInterrupt(1); }
    static void AHCI_InterruptHandler_C2(void *context) { AHCI::HandleInterrupt(2); }
    static void AHCI_InterruptHandler_C3(void *context) { AHCI::HandleInterrupt(3); }

    // Export handler table sized for MAX_AHCI_CONTROLLERS. Handlers take
    // a void* context per the new interrupt API.
    void (*g_ahci_handlers[MAX_AHCI_CONTROLLERS])(void *) = {
        AHCI_InterruptHandler_C0,
        AHCI_InterruptHandler_C1,
        AHCI_InterruptHandler_C2,
        AHCI_InterruptHandler_C3
    };

    VOID HandleInterrupt(VAL32 Controller_ID) {
        AHCIDriver &Driver = g_ahci_controllers[Controller_ID];
        volatile HBA_MEM* regs = Driver.regs;
        U32 PortsWithIRQ = regs->is;

        if (PortsWithIRQ == 0) return;

        for (U32 PortNum = 0; PortNum < 32; PortNum++) {
            if (!(PortsWithIRQ & (1 << PortNum))) continue;
            
            volatile HBA_PORT *port = &regs->ports[PortNum];
            U32 interesting = port->is & port->ie;

            // Acknowledge interrupt UNTUK PORT INI
            port->is = interesting;

            // Cek apakah transfer selesai (D2H FIS atau PIO Setup)
            if (interesting & ((1 << 0) | (1 << 1))) {
                Tasking::Task* waiting = Driver.WaitingTask[PortNum];
                if (waiting) {
                    Tasking::UnblockTaskWithIOBoost(waiting);
                    Driver.WaitingTask[PortNum] = nullptr; 
                }
            }
        }

        // Acknowledge global status
        regs->is = PortsWithIRQ;
    }
}
