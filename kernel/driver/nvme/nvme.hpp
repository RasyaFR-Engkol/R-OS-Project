#pragma once

#include <rosval.h>
#include "../../filesys/iblockdevice.hpp"
#include "../pci/pci.hpp"
#include "spinlock/mutex.hpp"

namespace NVMe{
    struct NVMeRegister{
        VOLATILE U64 CAP;    // Controller Capabilities (0x00)
        VOLATILE U32 VS;     // Version (0x08)
        VOLATILE U32 INTMS;  // Interrupt Mask Set (0x0C)
        VOLATILE U32 INTMC;  // Interrupt Mask Clear (0x10)
        VOLATILE U32 CC;     // Controller Configuration (0x14)
        VOLATILE U32 RSV0;   // Reserved
        VOLATILE U32 CSTS;   // Controller Status (0x1C)
        VOLATILE U32 NSSR;   // NVM Subsystem Reset (0x20)
        VOLATILE U32 AQA;    // Admin Queue Attributes (0x24)
        VOLATILE U64 ASQ;    // Admin Submission Queue Base Address (0x28)
        VOLATILE U64 ACQ;    // Admin Completion Queue Base Address (0x30)
        VOLATILE U32 CMBLOC; // Controller Memory Buffer Location (0x38)
        VOLATILE U32 CMBSZ;  // Controller Memory Buffer Size (0x3C)
        VOLATILE U32 BPINFO; // Boot Partition Info (0x40)
        VOLATILE U32 BPRSEL; // Boot Partition Read Select (0x44)
        VOLATILE U64 BPMBL;  // Boot Partition Memory Buffer Location (0x48)
        VOLATILE U64 CMSCS;  // Controller Memory Buffer Memory Space Control (0x50)
        
        // Sisanya padding sampai 0x1000
        U8 Padding[0x1000 - 0x58];
        
        // Mulai 0x1000 adalah Doorbell Registers
        // Format: Submission Tail DB (Queue 0), Completion Head DB (Queue 0), dst...
        // Ukurannya tergantung "Stride" di register CAP.
        VOLATILE U32 Doorbells[1024];
    } PACKSTRUCT;

    struct NVMeCommand{
        U8  Opcode;
        U8  Flags;
        U16 CommandID;
        U32 NSID;           // Namespace ID
        U64 Reserved;
        U64 MetadataPtr;
        U64 DataPtr1;       // Physical Address Buffer (PRP1)
        U64 DataPtr2;       // Physical Address Buffer (PRP2)
        U32 Dword10;
        U32 Dword11;
        U32 Dword12;
        U32 Dword13;
        U32 Dword14;
        U32 Dword15;
    } PACKSTRUCT;

    struct NVMeCompletion{
        U32 Dword0;
        U32 Reserved;
        U16 SQHead;         // Pointer posisi Submission Queue saat ini
        U16 SQID;           // ID Submission Queue asal
        U16 CommandID;      // ID Command yang selesai
        U16 Status;         // Status (Bit 0-14), Phase Tag (Bit 15)
    } PACKSTRUCT;

    class NVMeController : public IBlockDevice{
        private:
            U8 Bus, Device, Function;
            NVMeRegister* Regs;
            U8 Stride;

            NVMeCommand* AdminSQBase;
            NVMeCompletion* AdminCQBase;
            U64 AdminSQPhys;
            U64 AdminCQPhys;

            U16 AdminSQTail;
            U16 AdminCQHead;
            U8 PhaseTag;

            PageAlloc::DMAAlloc::DMABuffer* AdminSQBuf;
            PageAlloc::DMAAlloc::DMABuffer* AdminCQBuf;

            U8 InterruptVector;

            // namespace info
            U32 ActiveNSID;
            U64 LBACount;
            U32 LBASize;

            static void InterruptHandler(void* Context);
            
            // Logic benerannya disini
            void HandleInterrupt();

            VOID WriteDoorbell(U16 QueueID, U16 Value, BOOL IsCQ);
            BOOL PollAdminCompletion(U16 CID);

            U16 ProcessCompletionQueue(NVMeCompletion* CQBase, U16& Head, U8& PTag, U16 QueueID);
            BOOL SetupInterrupts();
            BOOL IdentifyNamespace();

            // I/O Queue (Kita bikin 1 pasang aja dulu: Queue ID 1)
            NVMeCommand* iOSQBase;
            NVMeCompletion* iOCQBase;
            U64 iOSQPhys;
            U64 iOCQPhys;
            
            U16 iOSQTail;
            U16 iOCQHead;
            U8  iOPhaseTag;
            
            PageAlloc::DMAAlloc::DMABuffer* iOSQBuf;
            PageAlloc::DMAAlloc::DMABuffer* iOCQBuf;

            // Method baru
            BOOL CreateIOQueues();

        public:
            NVMeController(U8 bus, U8 device, U8 function);
            Mutex IOLock;
            Tasking::Task *WaitingTask[32] = {nullptr};
            BOOL CommandStatus[32] = {FALSE};

            U16 AllocateCID(){
                for(U16 i = 0; i < 32; i++){
                    if(WaitingTask[i] == nullptr) return i;
                }
                return 0xFFFF;
            }
            
            BOOL Initialize();
            BOOL IdentifyController();

            virtual BOOL ReadSectors(U64 LBA, U32 Count, PageAlloc::DMAAlloc::DMABuffer **BufferOut) override;
            virtual BOOL WriteSectors(U64 LBA, U32 Count, PageAlloc::DMAAlloc::DMABuffer *Buffer) override;
            virtual BOOL FlushCache();
            virtual CONSTANT CHAR8* GetDeviceName() override {return "nvme0n1"; };

            static VOID RegisterController(U8 Bus, U8 Device, U8 Function);
    };

    extern NVMeController* g_NVMeController;
}