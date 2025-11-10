#include "debug.hpp"
#define PRINTK_MODULE_NAME "AHCIPORT"
#include <rosval.h>
#include "ahci.hpp"
#include "ahci_regs.hpp"
#include "rossys.hpp"
#include "logging.hpp"
#include "ahci_internal.hpp"

/* module name provided via PRINTK_MODULE_NAME */

namespace AHCI {

    static VOID StopPort(volatile HBA_PORT *Port){
        Port->cmd &= ~(1 << 0);
        Port->cmd &= ~(1 << 4);
        while ((Port->cmd & (1 << 15)) || (Port->cmd & (1 << 14))) {
            Arch::ASM::CPURelax();
        }
    }

    static void StartPort(volatile HBA_PORT* port) {
        while (port->cmd & (1 << 15)) {
            Arch::ASM::CPURelax();
        }
        port->cmd |= (1 << 4); // CMD.FRE = 1
        port->cmd |= (1 << 0); // CMD.ST = 1
    }

    BOOL InitializePort(AHCIDriver &Driver, int NumPort){
        volatile HBA_PORT *Port = &Driver.regs->ports[NumPort];

        StopPort(Port);

        Driver.dma_cmd_list[NumPort] = PageAlloc::DMAAlloc::AllocateDMAPages(1);
        if (!Driver.dma_cmd_list[NumPort]) {
            Printk::Write(Printk::Level::LOG_ERR, " Port %d: Failed to allocate DMA for Command List\n", NumPort);
            return FALSE;
        }

        Driver.dma_fis_buffers[NumPort] = PageAlloc::DMAAlloc::AllocateDMAPages(1);
        if (!Driver.dma_fis_buffers[NumPort]) {
            Printk::Write(Printk::Level::LOG_ERR, " Port %d: Failed to allocate DMA for FIS Buffer\n", NumPort);
            PageAlloc::DMAAlloc::FreeDMABuffer(Driver.dma_cmd_list[NumPort]);
            return FALSE;
        }

        Driver.dma_cmd_tables[NumPort] = PageAlloc::DMAAlloc::AllocateDMAPages(2);
        if(!Driver.dma_cmd_tables[NumPort]) {
            Printk::Write(Printk::Level::LOG_ERR, " Port %d: Failed to allocate DMA for Command Table\n", NumPort);
            PageAlloc::DMAAlloc::FreeDMABuffer(Driver.dma_cmd_list[NumPort]);
            PageAlloc::DMAAlloc::FreeDMABuffer(Driver.dma_fis_buffers[NumPort]);
            return FALSE;
        }

        LOCKRFLAGS _port_rflags = Arch::SaveAndDisableInterrupts();
        Port->clb = Driver.dma_cmd_list[NumPort]->PhysAddr;
        Port->fb = Driver.dma_fis_buffers[NumPort]->PhysAddr;

        Driver.v_cmd_lists[NumPort] = (volatile HBA_CMD_HEADER*)Driver.dma_cmd_list[NumPort]->VirtAddr;
        Driver.v_cmd_tables[NumPort] = (volatile U8*)Driver.dma_cmd_tables[NumPort]->VirtAddr;

        for(int i = 0; i < 32; i++) {
            volatile HBA_CMD_HEADER* cmd_header = &Driver.v_cmd_lists[NumPort][i];
            UPTR cmd_table_phys = Driver.dma_cmd_tables[NumPort]->PhysAddr + (i * 256);
            cmd_header->ctba = cmd_table_phys;
            cmd_header->prdbc = 0;
            cmd_header->prdtl = 0;
            cmd_header->cfl = 0;
        }

        Port->serr = (U32)-1; // Clear errors

        Arch::RestoreInterrupts(_port_rflags);

        StartPort(Port);
        return TRUE;
    }

    DeviceType ProbePort(AHCIDriver &Drv, VAL32 PortNum){
        volatile HBA_PORT *port = &Drv.regs->ports[PortNum];

        U32 SSTS = port->ssts;
        U8 DET = (U8)(SSTS & 0x0F);

        if(DET != 0x03) {
            Printk::Write(Printk::Level::LOG_INFO, " Port %d: No device detected (DET=%u)\n", (unsigned)PortNum, (unsigned)DET);
            return DeviceType::NONE;
        }

        U32 Sign = port->sig;

        switch(Sign){
            case 0x00000101:
                Printk::Write(Printk::Level::LOG_INFO, " Port %d: SATA device detected\n", (unsigned)PortNum);
                return DeviceType::SATA;
            case 0xEB140101:
                Printk::Write(Printk::Level::LOG_INFO, " Port %d: SEMB device detected\n", (unsigned)PortNum);
                return DeviceType::SEMB;
            case 0x96690101:
                Printk::Write(Printk::Level::LOG_INFO, " Port %d: Port Multiplier device detected\n", (unsigned)PortNum);
                return DeviceType::PM;
            case 0x00000002:
                Printk::Write(Printk::Level::LOG_INFO, " Port %d: SATAPI device detected\n", (unsigned)PortNum);
                return DeviceType::SATAPI;
            default:
                Printk::Write(Printk::Level::LOG_INFO, " Port %d: Unknown device signature: 0x%08X\n", (unsigned)PortNum, (unsigned)Sign);
                return DeviceType::NONE;
        }
    }

    VOID TestReadLBA0(){
        for (int i = 0; i < AHCI::g_ahci_controller_count; ++i) {
        AHCI::AHCIDriver &drv = AHCI::g_ahci_controllers[i];
        if (!drv.regs) continue;
        U32 pi = drv.regs->pi;
        for (int port = 0; port < 32; ++port) {
            if ((pi & (1u << port)) == 0) continue;
            Serial::Printf("[AHCI TEST] Trying READ LBA0 on ctrl %d port %d...\n", i, port);
            PageAlloc::DMAAlloc::DMABuffer *buf = nullptr;
            if (AHCI::ReadSectors(drv, port, /*lba*/1, /*count*/1, &buf)) {
                Serial::Write("[AHCI TEST] READ LBA0 OK, dumping 512B (phys+virt views):\n");
                // Sanity: verify virt->phys translation matches the DMA phys address
                UPTR chkPhys=0; U64 chkFlags=0; SIZE_T lvl=0;
                if (Debug::VirtToPhys((UPTR)buf->VirtAddr, &chkPhys, &chkFlags, &lvl)) {
                    Serial::Printf("[AHCI TEST] DMA buf virt=%p -> phys=%p flags=%llx lvl=%u\n",
                                   (void*)(uintptr_t)buf->VirtAddr, (void*)(uintptr_t)chkPhys,
                                   (unsigned long long)chkFlags, (unsigned)lvl);
                } else {
                    Serial::Printf("[AHCI TEST] VirtToPhys FAILED for %p\n", (void*)(uintptr_t)buf->VirtAddr);
                }
                // View 1: base printed as physical address
                Debug::HexDump((void*)(uintptr_t)buf->VirtAddr, 512, 16, buf->PhysAddr, true);
                // View 2: base printed as the virtual pointer
                Debug::HexDump((void*)(uintptr_t)buf->VirtAddr, 512, 16, 0, true);
                PageAlloc::DMAAlloc::FreeDMABuffer(buf);
                // Only test first successful port
                i = AHCI::g_ahci_controller_count; // break outer
                break;
            } else {
                Serial::Write("[AHCI TEST] READ LBA0 failed\n");
            }
        }
    }
    }

}
