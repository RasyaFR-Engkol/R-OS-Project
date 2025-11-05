#include <rosval.h>
#include "ahci.hpp"
#include "ahci_regs.hpp"
#include "logging.hpp"
#include "ahci_internal.hpp"

namespace AHCI {
    // ISR wrappers for controllers. Keep them small and map to HandleInterrupt
    static void AHCI_InterruptHandler_C0() { AHCI::HandleInterrupt(0); }
    static void AHCI_InterruptHandler_C1() { AHCI::HandleInterrupt(1); }
    static void AHCI_InterruptHandler_C2() { AHCI::HandleInterrupt(2); }
    static void AHCI_InterruptHandler_C3() { AHCI::HandleInterrupt(3); }

    // Export handler table sized for MAX_AHCI_CONTROLLERS
    void (*g_ahci_handlers[MAX_AHCI_CONTROLLERS])() = {
        AHCI_InterruptHandler_C0,
        AHCI_InterruptHandler_C1,
        AHCI_InterruptHandler_C2,
        AHCI_InterruptHandler_C3
    };

    VOID HandleInterrupt(VAL32 Controller_ID){
        Printk::Write(Printk::Level::LOG_CRIT, "[AHCI] Interrupt received from controller %u\n", (unsigned)Controller_ID);

        AHCIDriver &Driver = g_ahci_controllers[Controller_ID];
        volatile HBA_MEM* regs = Driver.regs;

        U32 PortsWithIRQ = regs->is; // Interrupt Status Register

        if(PortsWithIRQ == 0){
            Printk::Write(Printk::Level::LOG_WARNING, "[AHCI] Spurious interrupt on controller %u\n", (unsigned)Controller_ID);
            return;
        }

        Printk::Write(Printk::Level::LOG_INFO, "[AHCI] Controller %u - Ports with IRQ: 0x%08x\n",
            (unsigned)Controller_ID, (unsigned)PortsWithIRQ);

        U32 handled_ports_mask = 0;
        for(U32 PortNum = 0; PortNum < 32; PortNum++){
            if(!(PortsWithIRQ & (1 << PortNum))) continue;
            volatile HBA_PORT *port = &regs->ports[PortNum];

            U32 PortStatus = port->is;
            handled_ports_mask |= (1u << PortNum);

            if (Driver.port_device[PortNum] == DeviceType::NONE) {
                if (PortStatus) port->is = PortStatus; // acknowledge
                continue;
            }

            U32 enabled_mask = port->ie;
            U32 interesting = PortStatus & enabled_mask;
            if (!interesting) {
                if (PortStatus) port->is = PortStatus;
                continue;
            }

            port->is = interesting;

            Printk::Write(Printk::Level::LOG_INFO, "[AHCI] Controller %u Port %u - Port Status: 0x%08x\n",
                (unsigned)Controller_ID, (unsigned)PortNum, (unsigned)interesting);
        }

        if (handled_ports_mask) regs->is = handled_ports_mask;
    }

}
