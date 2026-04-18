#define PRINTK_MODULE_NAME "AHCICMDS"
#include <rosval.h>
#include "ahci.hpp"
#include "ahci_regs.hpp"
#include "rossys.hpp"
#include "string.hpp"
#include <port.hpp>
#include <mm.hpp>
#include <logging.hpp>
#include "../pic/timer/pit.hpp"
#include "ahci_internal.hpp"

/* module name provided via PRINTK_MODULE_NAME */

namespace AHCI {

    VAL32 FindFreeCommandSlot(AHCIDriver &Driver, VAL32 PortNum){
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

    static inline void BuildRWCFIS(FIS_REG_H2D* fis, U8 cmd, U64 lba, U16 count) {
        String::Memset(fis, 0, sizeof(FIS_REG_H2D));
        fis->fis_type = 0x27;           // H2D register FIS
        fis->pmport_c = 1u << 7;        // C bit = command
        fis->command = cmd;             // 0x25 read, 0x35 write
        fis->device = 1u << 6;          // LBA mode
        fis->lba0 = (U8)(lba & 0xFF);
        fis->lba1 = (U8)((lba >> 8) & 0xFF);
        fis->lba2 = (U8)((lba >> 16) & 0xFF);
        fis->lba3 = (U8)((lba >> 24) & 0xFF);
        fis->lba4 = (U8)((lba >> 32) & 0xFF);
        fis->lba5 = (U8)((lba >> 40) & 0xFF);
        fis->count = count;             // little-endian 16-bit sector count
    }

    static inline void InvalidateCacheLines(const void* ptr, U32 bytes) {
        const U8* p = (const U8*)ptr;
        const SIZE_T line = 64; // typical cacheline size
        UPTR addr = (UPTR)p;
        UPTR start = addr & ~(line - 1);
        SIZE_T total = (addr + bytes) - start;
        for (SIZE_T i = 0; i < total; i += line) {
            const void* a = (const void*)(start + i);
            asm volatile ("clflush (%0)" :: "r"(a) : "memory");
        }
        asm volatile ("mfence" ::: "memory");
    }

    static inline bool InterruptsEnabled() {
        unsigned long rflags;
        asm volatile ("pushfq; pop %0" : "=r"(rflags));
        return (rflags & (1UL << 9)) != 0; // IF bit
    }

    static BOOL IssueCommand(AHCIDriver &Drv, U32 PortNum, VAL32 Slot){
        volatile HBA_PORT *Port = &Drv.regs->ports[PortNum];

        Tasking::Task *CurrentTask = Tasking::GetCurrentTaskPtr();
        if(CurrentTask){
            Drv.WaitingTask[PortNum] = CurrentTask;
            CurrentTask->State = Tasking::TaskState::BLOCKED;
        }

        Port->ci = (1u << Slot);  // START COMMAND

        Tasking::SchedulerYield(); // Yield ke scheduler, nanti bakal dibangunin di interrupt handler

        Drv.WaitingTask[PortNum] = nullptr;

        // 4. Validasi Hasil (Sama seperti kodemu)
        if (Port->ci & (1u << Slot)) {
            Printk::Write(Printk::Level::LOG_ERR, " AHCI Port %u: Command timed out / Stuck (CI=0x%08X)\n", (unsigned)PortNum, (unsigned)Port->ci);
            Port->ci &= ~(1u << Slot); // Stop paksa
            return FALSE;
        }

        if (Port->is & (1u << 30)) {
            Printk::Write(Printk::Level::LOG_ERR, " AHCI Port %u: TFES Error (IS=0x%08X)\n", (unsigned)PortNum, (unsigned)Port->is);
            Port->is = 0xFFFFFFFF;
            return FALSE;
        }

        if (Port->tfd & 0x01) {
            Printk::Write(Printk::Level::LOG_ERR, " AHCI Port %u: TaskFile Error (TFD=0x%02X)\n", (unsigned)PortNum, (unsigned)Port->tfd);
            Port->is = 0xFFFFFFFF;
            return FALSE;
        }

        return TRUE;
    }

    BOOL SendIdentify(AHCIDriver &Driver, VAL32 PortNum){
        PageAlloc::DMAAlloc::DMABuffer *IDBuf = PageAlloc::DMAAlloc::AllocateDMAPages(1);
        if(!IDBuf){
            Printk::Write(Printk::Level::LOG_ERR, " Port %d: Failed to allocate DMA buffer for IDENTIFY\n", PortNum);
            return FALSE;
        }

        VAL32 Slot = FindFreeCommandSlot(Driver, PortNum);
        if(Slot == (VAL32)-1){
            Printk::Write(Printk::Level::LOG_ERR, " Port %d: No free command slot available for IDENTIFY\n", PortNum);
            PageAlloc::DMAAlloc::FreeDMABuffer(IDBuf);
            return FALSE;
        }

        volatile HBA_PORT *Port = &Driver.regs->ports[PortNum];
        HBA_CMD_HEADER *CmdHeader = (HBA_CMD_HEADER*)&Driver.v_cmd_lists[PortNum][Slot];
        HBA_CMD_TBL* CmdTable = (HBA_CMD_TBL*)(Driver.v_cmd_tables[PortNum] + (Slot * 256));

        CmdHeader->cfl = 5;
        CmdHeader->prdtl = 1;
        CmdHeader->w = 0;
        CmdHeader->prdbc = 0;

        CmdTable->prdt_entry[0].dba = IDBuf->PhysAddr;
        CmdTable->prdt_entry[0].dbc = 512 - 1; // 512 bytes

        FIS_REG_H2D* CmdFIS = (FIS_REG_H2D*)&CmdTable->cfis[0];
        String::Memset(CmdFIS, 0, sizeof(FIS_REG_H2D));
        CmdFIS->fis_type = 0x27; // H2D Register FIS
        CmdFIS->pmport_c = 1 << 7; // C bit = command
        CmdFIS->command = 0xEC; // IDENTIFY DEVICE
        CmdFIS->device = 0x00;

        LOCKRFLAGS _rf = Arch::SaveAndDisableInterrupts();
        Port->is = 0xFFFFFFFF;
        Port->ci = (1u << Slot);
        Arch::RestoreInterrupts(_rf);

        const U64 startTicks = PIT::ticks;
        const U64 TIMEOUT_TICKS = 500; // ~5s at 100Hz
        while ((Port->ci & (1u << Slot)) != 0) {
            U64 elapsed = PIT::ticks - startTicks;
            if (elapsed >= TIMEOUT_TICKS) break;
            Arch::ASM::CPURelax();
        }

        if ((Port->ci & (1u << Slot)) != 0) {
            Printk::Write(Printk::Level::LOG_ERR, " Port %d: IDENTIFY command timeout (slot still active)\n", PortNum);
            Port->ci &= ~(1u << Slot);
            PageAlloc::DMAAlloc::FreeDMABuffer(IDBuf);
            return FALSE;
        }

        if (Port->is & (1u << 30)) {
            Printk::Write(Printk::Level::LOG_ERR, " Port %d: IDENTIFY TFES set (IS=0x%08X)\n", PortNum, (unsigned)Port->is);
            Port->is = 0xFFFFFFFF; // acknowledge
            PageAlloc::DMAAlloc::FreeDMABuffer(IDBuf);
            return FALSE;
        }
        
        if(Port->tfd & 0x01) {
            Printk::Write(Printk::Level::LOG_ERR, " Port %d: IDENTIFY command failed (TFD=0x%02X)\n", PortNum, (unsigned)(Port->tfd));
            Port->is = 0xFFFFFFFF; // acknowledge
            PageAlloc::DMAAlloc::FreeDMABuffer(IDBuf);
            return FALSE;
        }

        if (Port->is) Port->is = 0xFFFFFFFF;

        //U16* IdentifyData = (U16*)IDBuf->VirtAddr;
        //U64 TotalSectors = ((U64)IdentifyData[100] | ((U64)IdentifyData[101] << 16) |
        //                    ((U64)IdentifyData[102] << 32) | ((U64)IdentifyData[103] << 48));
        
        //U64 TotalBytes = TotalSectors * 512;

        //Printk::Write(Printk::Level::LOG_DEBUG, " Port %d: IDENTIFY successful - Total Size: %llu bytes (%llu sectors)\n",
        //    PortNum, TotalBytes, TotalSectors);

        PageAlloc::DMAAlloc::FreeDMABuffer(IDBuf);
        return TRUE;
    }

    BOOL ReadSectors(AHCIDriver &Driver, VAL32 PortNum, U64 lba, U32 count,
                     PageAlloc::DMAAlloc::DMABuffer **outBuf) {

        Driver.PortLocks[PortNum].Acquire();

        if (!outBuf || count == 0) {
            Driver.PortLocks[PortNum].Release();
            return FALSE;
        }

        U32 bytes = count * 512u;
        SIZE_T pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
        PageAlloc::DMAAlloc::DMABuffer *buf = PageAlloc::DMAAlloc::AllocateDMAPages(pages);
        if (!buf) return FALSE;

        I32 Slot = FindFreeCommandSlot(Driver, PortNum);
        if (Slot == (VAL32)-1) { 
            PageAlloc::DMAAlloc::FreeDMABuffer(buf); 
            Driver.PortLocks[PortNum].Release(); 
            return FALSE; 
        }

        HBA_CMD_HEADER *hdr = (HBA_CMD_HEADER*)&Driver.v_cmd_lists[PortNum][Slot];
        HBA_CMD_TBL *tbl = (HBA_CMD_TBL*)(Driver.v_cmd_tables[PortNum] + (Slot * 256));

        hdr->cfl = 5;           // 20 bytes CFIS
        hdr->w = 0;             // read
        hdr->prdtl = 1;
        hdr->prdbc = 0;
        hdr->ctba = Driver.dma_cmd_tables[PortNum]->PhysAddr + (Slot * 256);

        tbl->prdt_entry[0].dba = buf->PhysAddr;
        tbl->prdt_entry[0].dbc = bytes - 1; // bytes-1


        BuildRWCFIS((FIS_REG_H2D*)&tbl->cfis[0], 0x25, lba, (U16)count);

        BOOL ok = IssueCommand(Driver, PortNum, Slot);
        if (!ok) { 
            PageAlloc::DMAAlloc::FreeDMABuffer(buf); 
            Driver.PortLocks[PortNum].Release(); 
            return FALSE; 
        }
        InvalidateCacheLines((const void*)(uintptr_t)buf->VirtAddr, bytes);

        Driver.PortLocks[PortNum].Release();

        *outBuf = buf;
        return TRUE;
    }

    BOOL FlushCache(AHCIDriver &Driver, VAL32 PortNum) {
        I32 Slot = FindFreeCommandSlot(Driver, PortNum);
        if (Slot == (VAL32)-1) {return FALSE; }

        HBA_CMD_HEADER *hdr = (HBA_CMD_HEADER*)&Driver.v_cmd_lists[PortNum][Slot];
        HBA_CMD_TBL *tbl = (HBA_CMD_TBL*)(Driver.v_cmd_tables[PortNum] + (Slot * 256));

        hdr->cfl = 5;           // 20 bytes CFIS
        hdr->w = 0;             // read
        hdr->prdtl = 0;
        hdr->prdbc = 0;
        hdr->ctba = Driver.dma_cmd_tables[PortNum]->PhysAddr + (Slot * 256);

        String::Memset(tbl, 0, sizeof(HBA_CMD_TBL));

        FIS_REG_H2D *CMDFis = (FIS_REG_H2D*)&tbl->cfis[0];
        String::Memset(CMDFis, 0, sizeof(FIS_REG_H2D));

        CMDFis->fis_type = 0x27; // H2D Register FIS
        CMDFis->pmport_c = 0x80; // C bit = command
        CMDFis->command = ATA_CMD_FLUSH_CACHE_EXT; // Flush Cache Extended
        CMDFis->device = 0x00;

        BOOL ok = IssueCommand(Driver, PortNum, Slot);
        if (!ok) {
            Printk::Write(Printk::Level::LOG_ERR, " AHCI Port %u: Flush Cache command failed\n", (unsigned)PortNum);
            return FALSE;
        }

        return TRUE;
    }

    BOOL WriteSectors(AHCIDriver &Driver, VAL32 PortNum, U64 lba, U32 count,
                      PageAlloc::DMAAlloc::DMABuffer *buf) 
    {
        Driver.PortLocks[PortNum].Acquire();

        if (!buf || count == 0) {
            Driver.PortLocks[PortNum].Release();
            return FALSE;
        }

        U32 bytes = count * 512u;
        if (bytes > buf->Size) {
            Printk::Write(Printk::Level::LOG_ERR, " Write: buffer too small (need %u have %u)\n",
                          (unsigned)bytes, (unsigned)buf->Size);
            return FALSE;
        }

        I32 Slot = FindFreeCommandSlot(Driver, PortNum);
        if (Slot == (VAL32)-1) return FALSE;

        HBA_CMD_HEADER *hdr = (HBA_CMD_HEADER*)&Driver.v_cmd_lists[PortNum][Slot];
        HBA_CMD_TBL *tbl = (HBA_CMD_TBL*)(Driver.v_cmd_tables[PortNum] + (Slot * 256));

        hdr->cfl = 5;           // 20 bytes CFIS
        hdr->w = 1;             // write
        hdr->prdtl = 1;
        hdr->prdbc = 0;
        hdr->ctba = Driver.dma_cmd_tables[PortNum]->PhysAddr + (Slot * 256);

        tbl->prdt_entry[0].dba = buf->PhysAddr;
        // Request interrupt on completion (IOC) for writes as well.
        tbl->prdt_entry[0].dbc = (bytes - 1) | (1u << 31);

        BuildRWCFIS((FIS_REG_H2D*)&tbl->cfis[0], 0x35, lba, (U16)count);

        BOOL ok = IssueCommand(Driver, PortNum, Slot);

        Driver.PortLocks[PortNum].Release();

        return ok;
    }

}
