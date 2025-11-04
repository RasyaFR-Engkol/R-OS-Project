#include <rosval.h>
#include "ahci.hpp"
#include "ahci_regs.hpp"
#include "rossys.hpp"
#include "string.hpp"
#include <port.hpp>
#include <mm.hpp>
#include <logging.hpp>
#include "../pic/timer/pit.hpp"
#include "../pci/capatibility/msixmsi/msixmsi.hpp"

namespace AHCI{
    AHCIDriver g_ahci_controllers[MAX_AHCI_CONTROLLERS];
    int g_ahci_controller_count = 0;

    // Ini adalah ISR-nya. Harus statik/global.
    // Kita pakai trik 'controller_id' untuk tahu mana yang interrupt
    static void AHCI_InterruptHandler_C0() { AHCI::HandleInterrupt(0); }
    static void AHCI_InterruptHandler_C1() { AHCI::HandleInterrupt(1); }
    static void AHCI_InterruptHandler_C2() { AHCI::HandleInterrupt(2); }
    static void AHCI_InterruptHandler_C3() { AHCI::HandleInterrupt(3); }

    static void (*g_ahci_handlers[4])() = {
        AHCI_InterruptHandler_C0,
        AHCI_InterruptHandler_C1,
        AHCI_InterruptHandler_C2,
        AHCI_InterruptHandler_C3
    };

    VOID HandleInterrupt(VAL32 Controller_ID){
        Printk::Write(Printk::Level::LOG_CRIT, "[AHCI] Interrupt received from controller %u\n", (unsigned)Controller_ID);

        AHCIDriver &Driver = g_ahci_controllers[Controller_ID];
        volatile HBA_MEM* regs = Driver.regs;

        U32 PortsWithIRQ = regs->is; // Interrupt Status Ruegister

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

            // If no device present on this port, ack and skip verbose logging.
            if (Driver.port_device[PortNum] == DeviceType::NONE) {
                if (PortStatus) port->is = PortStatus; // acknowledge
                continue;
            }

            // Mask to only the bits we enabled for the port (avoid noise)
            U32 enabled_mask = port->ie;
            U32 interesting = PortStatus & enabled_mask;
            if (!interesting) {
                // Nothing interesting for this port; ack whatever was set to avoid spam
                if (PortStatus) port->is = PortStatus;
                continue;
            }

            // Acknowledge only the bits we handled
            port->is = interesting;

            Printk::Write(Printk::Level::LOG_INFO, "[AHCI] Controller %u Port %u - Port Status: 0x%08x\n",
                (unsigned)Controller_ID, (unsigned)PortNum, (unsigned)interesting);
        }

        // Clear controller-level status bits for ports that had IRQs
        if (handled_ports_mask) regs->is = handled_ports_mask;
    }

    VOID RegisterAHCIController(U8 Bus, U8 Device, U8 Function, U8 MSICapOffset){
        if(g_ahci_controller_count >= MAX_AHCI_CONTROLLERS){
            return; // ngapain 4 AHCI gitu bang? 🤨
        }

    U32 ABAR_RAW = PCI::ReadDword(Bus, Device, Function, 0x24);
    UPTR ABAR_Phys = (UPTR)(ABAR_RAW & 0xFFFFFFF0);
    // Ensure the physical base is page-aligned before mapping. PCI BARs may
    // have low-order bits used for flags; MapPages expects a page-aligned
    // physical address (it writes the value directly into the PTE), so
    // align down to avoid introducing reserved bits into the page table.
    UPTR ABAR_PhysPage = ABAR_Phys & ~(PAGE_SIZE - 1);
        
        VOID* VirtAddr = PageAlloc::VirtualAllocPages(1);
        if(!VirtAddr) {
            return;
        }

        // Do not set PAGE_NX here: setting the NX bit requires IA32_EFER.NXE to
        // be enabled. On some platforms/early boot paths NXE may be clear and
        // writing bit 63 into a PTE causes a reserved-bit #PF (PF code 0x9).
        // Avoid NX for device MMIO mappings; we can enable NXE globally later
        // if desired and then set NX for code pages explicitly.
        PFLAGS Flags = PAGE_PRESENT | PAGE_RW | PAGE_PCD;
        if(!PageAlloc::MapPages(KernelPML4, ABAR_PhysPage, (UPTR)VirtAddr, 1, Flags)) {
            return;
        }

    // Disable interrupts briefly while we populate the driver state and
    // program initial device configuration to avoid races with any
    // in-flight IRQs from this device during setup.
    LOCKRFLAGS _ahci_rflags = Arch::SaveAndDisableInterrupts();
    AHCIDriver &DRV = g_ahci_controllers[g_ahci_controller_count];
    DRV.regs = (volatile HBA_MEM*)VirtAddr;
    DRV.bus = Bus;
    DRV.dev = Device;
    DRV.func = Function;
    DRV.initialized = FALSE;
    DRV.IntVector = 0; // will be set during initialization

        if(MSICapOffset != 0){
            VOID (*MyHandler)(VOID) = g_ahci_handlers[g_ahci_controller_count];

            U8 Vector = MSI::EnableMSI(Bus, Device, Function, MSICapOffset, MyHandler);
            if(Vector != 0){
                DRV.IntVector = Vector;
                Printk::Write(Printk::Level::LOG_INFO, "[AHCI] Enabled MSI on AHCI Controller %02X:%02X:%02X with vector 0x%02x\n",
                    (unsigned)Bus, (unsigned)Device, (unsigned)Function, (unsigned)Vector);
            } else {
                Printk::Write(Printk::Level::LOG_ERR, "[AHCI] Failed to enable MSI on AHCI Controller %02X:%02X:%02X\n",
                    (unsigned)Bus, (unsigned)Device, (unsigned)Function);
            }
        }

    g_ahci_controller_count++;

    // Restore original interrupt flags after controller registration
    Arch::RestoreInterrupts(_ahci_rflags);

        Printk::Write(Printk::Level::LOG_INFO, "[AHCI] Registered AHCI Controller at %02X:%02X:%02X, ABAR phys=%p virt=%p\n",
            (unsigned)Bus, (unsigned)Device, (unsigned)Function,
            (void*)(uintptr_t)ABAR_Phys, VirtAddr);
    }

    static VOID StopPort(volatile HBA_PORT *Port){
        // Clear BIT
        Port->cmd &= ~(1 << 0);
        Port->cmd &= ~(1 << 4);
        // Wait until CR is cleared
        while ((Port->cmd & (1 << 15)) || (Port->cmd & (1 << 14))) {
            Arch::ASM::CPURelax();
        }
    }

    // Mulai menjalankan port
    static void StartPort(volatile HBA_PORT* port) {
        // Tunggu sampai CR (Command List Running) mati
        while (port->cmd & (1 << 15)) {
            // Tunggu...
        }
        
        // Set bit FRE (FIS Receive Enable) dan ST (Start)
        port->cmd |= (1 << 4); // CMD.FRE = 1
        port->cmd |= (1 << 0); // CMD.ST = 1
    }

    static BOOL InitializePort(AHCIDriver &Driver, int NumPort){
        volatile HBA_PORT *Port = &Driver.regs->ports[NumPort];

        StopPort(Port);

        Driver.dma_cmd_list[NumPort] = PageAlloc::DMAAlloc::AllocateDMAPages(1);
        if (!Driver.dma_cmd_list[NumPort]) {
            Printk::Write(Printk::Level::LOG_ERR, "[AHCI] Port %d: Failed to allocate DMA for Command List\n", NumPort);
            return FALSE;
        }

        Driver.dma_fis_buffers[NumPort] = PageAlloc::DMAAlloc::AllocateDMAPages(1);
        if (!Driver.dma_fis_buffers[NumPort]) {
            Printk::Write(Printk::Level::LOG_ERR, "[AHCI] Port %d: Failed to allocate DMA for FIS Buffer\n", NumPort);
            PageAlloc::DMAAlloc::FreeDMABuffer(Driver.dma_cmd_list[NumPort]);
            return FALSE;
        }

        Driver.dma_cmd_tables[NumPort] = PageAlloc::DMAAlloc::AllocateDMAPages(2);
        if(!Driver.dma_cmd_tables[NumPort]) {
            Printk::Write(Printk::Level::LOG_ERR, "[AHCI] Port %d: Failed to allocate DMA for Command Table\n", NumPort);
            PageAlloc::DMAAlloc::FreeDMABuffer(Driver.dma_cmd_list[NumPort]);
            PageAlloc::DMAAlloc::FreeDMABuffer(Driver.dma_fis_buffers[NumPort]);
            return FALSE;
        }

    // Protect register writes from racing with interrupts by briefly
    // disabling interrupts while configuring the port registers.
    LOCKRFLAGS _port_rflags = Arch::SaveAndDisableInterrupts();
    Port->clb = Driver.dma_cmd_list[NumPort]->PhysAddr;
    Port->fb = Driver.dma_fis_buffers[NumPort]->PhysAddr;

        Driver.v_cmd_lists[NumPort] = (volatile HBA_CMD_HEADER*)Driver.dma_cmd_list[NumPort]->VirtAddr;

        Driver.v_cmd_tables[NumPort] = (volatile U8*)Driver.dma_cmd_tables[NumPort]->VirtAddr;

        for(int i = 0; i < 32; i++) {
            volatile HBA_CMD_HEADER* cmd_header = &Driver.v_cmd_lists[NumPort][i];
        
            // Alamat fisik dari Command Table ke-i
            UPTR cmd_table_phys = Driver.dma_cmd_tables[NumPort]->PhysAddr + (i * 256);
            
            // Beri tahu Command Header alamat fisiknya
            cmd_header->ctba = cmd_table_phys;
            
            // (Bersihkan juga header-nya)
            cmd_header->prdbc = 0;
            cmd_header->prdtl = 0;
            cmd_header->cfl = 0;
        }

        Port->serr = (U32)-1; // Clear errors

        if (Driver.IntVector != 0) {
            // Aktifkan interrupt di level PORT
            // (Bit 0 = D2H, Bit 1 = PIOS, Bit 30 = Fatal Error)
            Port->ie = (1 << 0) | (1 << 1) | (1 << 30);
        }

        // Restore interrupts once port configuration is done
        Arch::RestoreInterrupts(_port_rflags);

        StartPort(Port);
        return TRUE;
    }

    static DeviceType ProbePort(AHCIDriver &Drv, VAL32 PortNum){
        volatile HBA_PORT *port = &Drv.regs->ports[PortNum];

        U32 SSTS = port->ssts;
        U8 DET = (U8)(SSTS & 0x0F);

        if(DET != 0x03) {
            Printk::Write(Printk::Level::LOG_INFO, "[AHCI] Port %d: No device detected (DET=%u)\n", (unsigned)PortNum, (unsigned)DET);
            return DeviceType::NONE;
        }

        U32 Sign = port->sig;

        switch(Sign){
            case 0x00000101:
                Printk::Write(Printk::Level::LOG_INFO, "[AHCI] Port %d: SATA device detected\n", (unsigned)PortNum);
                return DeviceType::SATA;
            case 0xEB140101:
                Printk::Write(Printk::Level::LOG_INFO, "[AHCI] Port %d: SEMB device detected\n", (unsigned)PortNum);
                return DeviceType::SEMB;
            case 0x96690101:
                Printk::Write(Printk::Level::LOG_INFO, "[AHCI] Port %d: Port Multiplier device detected\n", (unsigned)PortNum);
                return DeviceType::PM;
            case 0x00000002:
                Printk::Write(Printk::Level::LOG_INFO, "[AHCI] Port %d: SATAPI device detected\n", (unsigned)PortNum);
                return DeviceType::SATAPI;
            default:
                Printk::Write(Printk::Level::LOG_INFO, "[AHCI] Port %d: Unknown device signature: 0x%08X\n", (unsigned)PortNum, (unsigned)Sign);
                return DeviceType::NONE;
        }
    }

    VOID InitializeAllControllers() {
        Printk::Write(Printk::Level::LOG_NOTICE, "[AHCI] Initializing all AHCI controllers (%d found)\n", g_ahci_controller_count);

        for(int i = 0; i < g_ahci_controller_count; i++) {
            AHCIDriver &DRV = g_ahci_controllers[i];

            U32 PortImplemented = DRV.regs->pi;

            Printk::Write(Printk::Level::LOG_INFO, "[AHCI] Controller %d at %02X:%02X:%02X - Ports Implemented: 0x%08X\n",
                i, (unsigned)DRV.bus, (unsigned)DRV.dev, (unsigned)DRV.func,
                (unsigned)PortImplemented);

            for(int portnum = 0; portnum < 32; portnum++) {
                if(PortImplemented & (1 << portnum)) {
                    if(InitializePort(DRV, portnum)) {
                        DeviceType DevType = ProbePort(DRV, portnum);
                        if(DevType == DeviceType::SATA) {
                            SendIdentify(DRV, portnum);
                        }
                        // Record detected device type per-port so IRQ handler can
                        // ignore interrupts from empty ports
                        DRV.port_device[portnum] = DevType;
                    } else {
                        Printk::Write(Printk::Level::LOG_ERR, "[AHCI] Controller %d Port %d failed to initialize\n", i, portnum);
                    }
                }
            }

            DRV.initialized = TRUE;
            if (DRV.IntVector != 0) {
                // Aktifkan interrupt di level KONTROLLER (Global)
                DRV.regs->ghc |= (1 << 1); // GHC.IE (Interrupt Enable)
            }
        }
    }

    static VAL32 FindFreeCommandSlot(AHCIDriver &Driver, VAL32 PortNum){
        volatile HBA_PORT *Port = &Driver.regs->ports[PortNum];

        U32 Slots = (Port->ci | Port->sact);

        for(int i = 0; i < 32; i++){
            if((Slots & 1) == 0){
                return i;
            }
            Slots >>= 1;
        }
        return -1;
    }

    // Helper: build H2D Register FIS for READ/WRITE DMA EXT
    static inline void BuildRWCFIS(FIS_REG_H2D* fis, U8 cmd, U64 lba, U16 count) {
        String::Memset(fis, 0, sizeof(FIS_REG_H2D));
        fis->fis_type = 0x27;           // H2D register FIS
        fis->pmport_c = 1u << 7;        // C bit = command
        fis->command = cmd;             // 0x25 read, 0x35 write
        fis->device = 1u << 6;          // LBA mode
        // LBA 48-bit
        fis->lba0 = (U8)(lba & 0xFF);
        fis->lba1 = (U8)((lba >> 8) & 0xFF);
        fis->lba2 = (U8)((lba >> 16) & 0xFF);
        fis->lba3 = (U8)((lba >> 24) & 0xFF);
        fis->lba4 = (U8)((lba >> 32) & 0xFF);
        fis->lba5 = (U8)((lba >> 40) & 0xFF);
        fis->count = count;             // little-endian 16-bit sector count
    }

    // On some platforms, DMA writes may not be visible if CPU has cached the
    // buffer (we memset to 0 before issuing). While x86 is typically cache-coherent,
    // add a conservative CLFLUSH over the read range to avoid reading stale zeros.
    static inline void InvalidateCacheLines(const void* ptr, U32 bytes) {
    const U8* p = (const U8*)ptr;
    const SIZE_T line = 64; // typical cacheline size
    // Align start down to cacheline
    UPTR addr = (UPTR)p;
        UPTR start = addr & ~(line - 1);
        SIZE_T total = (addr + bytes) - start;
        for (SIZE_T i = 0; i < total; i += line) {
            const void* a = (const void*)(start + i);
            asm volatile ("clflush (%0)" :: "r"(a) : "memory");
        }
        asm volatile ("mfence" ::: "memory");
    }

    // Helper: issue command in slot and wait with PIT-based timeout; returns TRUE on success
        // Helper: issue command in slot and wait for completion.
        // If interrupts are enabled (IF in RFLAGS), we sleep with HLT and let the
        // AHCI/MSI interrupt wake us, re-checking CI until the slot clears.
        // If interrupts are disabled (e.g., early init), we fallback to a PIT-timed
        // busy-wait with a timeout.
        static inline bool InterruptsEnabled() {
            unsigned long rflags;
            asm volatile ("pushfq; pop %0" : "=r"(rflags));
            return (rflags & (1UL << 9)) != 0; // IF bit
        }

        static BOOL IssueAndWait(volatile HBA_PORT* Port, VAL32 Slot) {
        // Clear pending interrupts
        Port->is = 0xFFFFFFFF;
        // Issue
        Port->ci = (1u << Slot);
        const U64 start = PIT::ticks;
        const U64 TIMEOUT_TICKS = 200; // ~2s at 100Hz
            if (InterruptsEnabled()) {
                // Sleep until an interrupt wakes us; re-check CI in case of spurious IRQs
                while (Port->ci & (1u << Slot)) {
                    asm volatile ("hlt");
                }
            } else {
                // Early init path: use timed busy-wait to avoid hanging when IF=0
                while (Port->ci & (1u << Slot)) {
                    if ((PIT::ticks - start) > TIMEOUT_TICKS) {
                        Printk::Write(Printk::Level::LOG_ERR, "[AHCI] IssueAndWait timeout (IF=0)\n");
                        break;
                    }
                }
            }
        if (Port->ci & (1u << Slot)) {
            Printk::Write(Printk::Level::LOG_ERR, "[AHCI] Timeout waiting slot %u to complete (CI=0x%08X)\n",
                          (unsigned)Slot, (unsigned)Port->ci);
            Port->ci &= ~(1u << Slot);
            return FALSE;
        }
        // Fatal/TF error?
        if (Port->is & (1u << 30)) {
            Printk::Write(Printk::Level::LOG_ERR, "[AHCI] TFES set after command (IS=0x%08X)\n", (unsigned)Port->is);
            Port->is = 0xFFFFFFFF;
            return FALSE;
        }
        if (Port->tfd & 0x01) {
            Printk::Write(Printk::Level::LOG_ERR, "[AHCI] TaskFile ERR after command (TFD=0x%02X)\n", (unsigned)Port->tfd);
            Port->is = 0xFFFFFFFF;
            return FALSE;
        }
        // Ack leftovers
        if (Port->is) Port->is = 0xFFFFFFFF;
        return TRUE;
    }

    BOOL SendIdentify(AHCIDriver &Driver, VAL32 PortNum){
        PageAlloc::DMAAlloc::DMABuffer *IDBuf = PageAlloc::DMAAlloc::AllocateDMAPages(1);
        if(!IDBuf){
            Printk::Write(Printk::Level::LOG_ERR, "[AHCI] Port %d: Failed to allocate DMA buffer for IDENTIFY\n", PortNum);
            return FALSE;
        }

        VAL32 Slot = FindFreeCommandSlot(Driver, PortNum);
        if(Slot == (VAL32)-1){
            Printk::Write(Printk::Level::LOG_ERR, "[AHCI] Port %d: No free command slot available for IDENTIFY\n", PortNum);
            PageAlloc::DMAAlloc::FreeDMABuffer(IDBuf);
            return FALSE;
        }

        volatile HBA_PORT *Port = &Driver.regs->ports[PortNum];
        volatile HBA_CMD_HEADER *CmdHeader = &Driver.v_cmd_lists[PortNum][Slot];
        volatile HBA_CMD_TBL* CmdTable = (volatile HBA_CMD_TBL*)
            (Driver.v_cmd_tables[PortNum] + (Slot * 256)); // (256B per table)

    // CFIS length in DWORDs (first 20 bytes of H2D FIS are significant) => 5 dwords
    CmdHeader->cfl = 5;
        CmdHeader->prdtl = 1;
        CmdHeader->w = 0;
        CmdHeader->prdbc = 0;

    CmdTable->prdt_entry[0].dba = IDBuf->PhysAddr;
    // dbc is byte count - 1 (lower 31 bits), bit0 may be interrupt flag depending on layout.
    CmdTable->prdt_entry[0].dbc = 512 - 1; // 512 bytes

    // Prepare a Register - Host to Device FIS for IDENTIFY DEVICE (ATA command 0xEC)
    FIS_REG_H2D* CmdFIS = (FIS_REG_H2D*)&CmdTable->cfis[0];
    String::Memset(CmdFIS, 0, sizeof(FIS_REG_H2D));
    CmdFIS->fis_type = 0x27; // H2D Register FIS
    CmdFIS->pmport_c = 1 << 7; // C bit = command
    CmdFIS->command = 0xEC; // IDENTIFY DEVICE
    CmdFIS->device = 0x00;

        // Atomically clear pending interrupts and issue the command while
        // interrupts are disabled to avoid races where an IRQ fires between
        // clearing IS and setting CI. We restore interrupts before waiting
        // since IssueAndWait expects interrupts enabled to use HLT.
        LOCKRFLAGS _rf = Arch::SaveAndDisableInterrupts();
        Port->is = 0xFFFFFFFF;
        Port->ci = (1u << Slot);
        Arch::RestoreInterrupts(_rf);

        // Wait for completion with a PIT-based timeout (approx 2 seconds at 100Hz)
        const U64 startTicks = PIT::ticks;
        const U64 TIMEOUT_TICKS = 200; // ~2s at 100Hz
        while ((Port->ci & (1u << Slot)) != 0) {
            U64 elapsed = PIT::ticks - startTicks;
            if (elapsed >= TIMEOUT_TICKS) break;
            Arch::ASM::CPURelax();
        }

        if ((Port->ci & (1u << Slot)) != 0) {
            Printk::Write(Printk::Level::LOG_ERR, "[AHCI] Port %d: IDENTIFY command timeout (slot still active)\n", PortNum);
            // Attempt to clear the slot so it doesn't block others
            Port->ci &= ~(1u << Slot);
            PageAlloc::DMAAlloc::FreeDMABuffer(IDBuf);
            return FALSE;
        }

        // Check Task File and Port Error bits; non-zero IS doesn't necessarily mean error (PIO/DMA setup FIS)
        if (Port->is & (1u << 30)) { // TFES: Task File Error Status
            Printk::Write(Printk::Level::LOG_ERR, "[AHCI] Port %d: IDENTIFY TFES set (IS=0x%08X)\n", PortNum, (unsigned)Port->is);
            Port->is = 0xFFFFFFFF; // acknowledge
            PageAlloc::DMAAlloc::FreeDMABuffer(IDBuf);
            return FALSE;
        }
        
        if(Port->tfd & 0x01) {
            Printk::Write(Printk::Level::LOG_ERR, "[AHCI] Port %d: IDENTIFY command failed (TFD=0x%02X)\n", PortNum, (unsigned)(Port->tfd));
            Port->is = 0xFFFFFFFF; // acknowledge
            PageAlloc::DMAAlloc::FreeDMABuffer(IDBuf);
            return FALSE;
        }

        // Acknowledge any remaining interrupt bits
        if (Port->is) Port->is = 0xFFFFFFFF;

        U16* IdentifyData = (U16*)IDBuf->VirtAddr;
        U64 TotalSectors = ((U64)IdentifyData[100] | ((U64)IdentifyData[101] << 16) |
                            ((U64)IdentifyData[102] << 32) | ((U64)IdentifyData[103] << 48));
        
        U64 TotalBytes = TotalSectors * 512;

        Printk::Write(Printk::Level::LOG_INFO, "[AHCI] Port %d: IDENTIFY successful - Total Size: %llu bytes (%llu sectors)\n",
            PortNum, TotalBytes, TotalSectors);

        PageAlloc::DMAAlloc::FreeDMABuffer(IDBuf);
        return TRUE;
    }

    BOOL ReadSectors(AHCIDriver &Driver, VAL32 PortNum, U64 lba, U32 count,
                     PageAlloc::DMAAlloc::DMABuffer **outBuf) {
        if (!outBuf || count == 0) return FALSE;
        volatile HBA_PORT *Port = &Driver.regs->ports[PortNum];

        // Allocate a DMA buffer big enough for count*512 bytes
        U32 bytes = count * 512u;
        SIZE_T pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
        PageAlloc::DMAAlloc::DMABuffer *buf = PageAlloc::DMAAlloc::AllocateDMAPages(pages);
        if (!buf) return FALSE;

        VAL32 Slot = FindFreeCommandSlot(Driver, PortNum);
        if (Slot == (VAL32)-1) { PageAlloc::DMAAlloc::FreeDMABuffer(buf); return FALSE; }

        volatile HBA_CMD_HEADER *hdr = &Driver.v_cmd_lists[PortNum][Slot];
        volatile HBA_CMD_TBL *tbl = (volatile HBA_CMD_TBL*)(Driver.v_cmd_tables[PortNum] + (Slot * 256));

        // Setup header
        hdr->cfl = 5;           // 20 bytes CFIS
        hdr->w = 0;             // read
        hdr->prdtl = 1;
        hdr->prdbc = 0;
        hdr->ctba = Driver.dma_cmd_tables[PortNum]->PhysAddr + (Slot * 256);

        // Setup PRDT
        tbl->prdt_entry[0].dba = buf->PhysAddr;
        tbl->prdt_entry[0].dbc = bytes - 1; // bytes-1

        // Build CFIS
        BuildRWCFIS((FIS_REG_H2D*)&tbl->cfis[0], 0x25, lba, (U16)count);

    // Issue the command: clear prior IS + set CI atomically to avoid races
    LOCKRFLAGS _rf = Arch::SaveAndDisableInterrupts();
    Port->is = 0xFFFFFFFF;
    Port->ci = (1u << Slot);
    Arch::RestoreInterrupts(_rf);

    BOOL ok = IssueAndWait(Port, Slot);
        if (!ok) { PageAlloc::DMAAlloc::FreeDMABuffer(buf); return FALSE; }
        InvalidateCacheLines((const void*)(uintptr_t)buf->VirtAddr, bytes);

        *outBuf = buf;
        return TRUE;
    }

    BOOL WriteSectors(AHCIDriver &Driver, VAL32 PortNum, U64 lba, U32 count,
                      PageAlloc::DMAAlloc::DMABuffer *buf) {
        if (!buf || count == 0) return FALSE;
        volatile HBA_PORT *Port = &Driver.regs->ports[PortNum];

        U32 bytes = count * 512u;
        if (bytes > buf->Size) {
            Printk::Write(Printk::Level::LOG_ERR, "[AHCI] Write: buffer too small (need %u have %u)\n",
                          (unsigned)bytes, (unsigned)buf->Size);
            return FALSE;
        }

        VAL32 Slot = FindFreeCommandSlot(Driver, PortNum);
        if (Slot == (VAL32)-1) return FALSE;

        volatile HBA_CMD_HEADER *hdr = &Driver.v_cmd_lists[PortNum][Slot];
        volatile HBA_CMD_TBL *tbl = (volatile HBA_CMD_TBL*)(Driver.v_cmd_tables[PortNum] + (Slot * 256));

        // Setup header
        hdr->cfl = 5;           // 20 bytes CFIS
        hdr->w = 1;             // write
        hdr->prdtl = 1;
        hdr->prdbc = 0;
        hdr->ctba = Driver.dma_cmd_tables[PortNum]->PhysAddr + (Slot * 256);

        // Setup PRDT
        tbl->prdt_entry[0].dba = buf->PhysAddr;
        tbl->prdt_entry[0].dbc = bytes - 1; // bytes-1

    // Build CFIS
    BuildRWCFIS((FIS_REG_H2D*)&tbl->cfis[0], 0x35, lba, (U16)count);

    // Issue the command: clear prior IS + set CI atomically to avoid races
    LOCKRFLAGS _rf = Arch::SaveAndDisableInterrupts();
    Port->is = 0xFFFFFFFF;
    Port->ci = (1u << Slot);
    Arch::RestoreInterrupts(_rf);

    BOOL ok = IssueAndWait(Port, Slot);
        return ok;
    }


    
}