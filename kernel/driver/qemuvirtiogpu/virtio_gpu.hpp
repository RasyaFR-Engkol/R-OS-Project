#pragma once
#include <rosval.h>
#include "virtio_structs.hpp"
#include <mm.hpp>

namespace VirtioGPU{
    struct VirtQueue{
        PageAlloc::DMAAlloc::DMABuffer* DescBuf;
        PageAlloc::DMAAlloc::DMABuffer* AvailBuf;
        PageAlloc::DMAAlloc::DMABuffer* UsedBuf;

        VRingDesc* Desc;
        VRingAvail* Avail;
        VRingUsed* Used;

        U16 QueueSize;
        U16 LastUsedIdx;
        U16 AvailableIdx;
        
        U16 NotifyOffset;
    };

    class Driver{
        private:
            U8 Bus, Dev, Function;

            VirtioPciCommonCfg* CommonCfg;
            U8* NotifyBase;
            U32 NotifyMultiplier;
            U8* DeviceCfg;

            VirtQueue Queues[2];

            // Internal Helpers
            void ParseCapabilities();
            void SetupQueue(int queueIndex);
            void NotifyQueue(int queueIndex);
        
            // Command Helpers
            // Mengirim command synchronous (tunggu sampai selesai)
            // Note: Idealnya async pake interrupt, tapi buat init pake sync dulu.
            BOOL SendCommand(U64 PhysCmd, U32 cmdSize, U64 PhysResp, U32 respSize);
            PageAlloc::DMAAlloc::DMABuffer* CmdBuffer;
            U32 CurrentWidth;
            VOID FlushEvents();
        public:
            void Initialize(U8 bus, U8 dev, U8 func);
            static void HardwareFlush(U32 x, U32 y, U32 w, U32 h);
            // Fungsi utama setup layar
            void SetupScanout(U32 width, U32 height);
    };

    extern Driver GlobalDriver;
}