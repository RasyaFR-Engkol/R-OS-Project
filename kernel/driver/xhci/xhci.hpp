#pragma once
#include "xhci_regs.hpp"
#include <rossys.hpp>
#include <rosval.h>
#include "xhci_regs.hpp" // Uncomment this when we need it
#include <mm.hpp>

namespace xHCI{
    #define XHCI_MAX_CONTROLLERS 4

    struct xHCIDriver{
        volatile U8 *regs_base;
        U8 bus, dev, func;
        U8 IntVector;
        BOOL Initialized;

        // Pointer-pointer yang sudah di-parse
        volatile xHCICapRegisters* cap_regs;
        volatile xHCIOpRegisters* op_regs;
        volatile U32* doorbell_regs;

        volatile xHCIRuntimeRegisters *rt_regs;
    volatile xHCIPortRegs *port_regs; // Pointer to port registers array (OpRegs + 0x400)
    U8 PortCount;                    // Number of root hub ports (from HCS1)

        PageAlloc::DMAAlloc::DMABuffer* DMA_DCBAAP;
        PageAlloc::DMAAlloc::DMABuffer* DMA_EventRing;
        PageAlloc::DMAAlloc::DMABuffer* DMA_CmdRing;
        PageAlloc::DMAAlloc::DMABuffer* DMA_ERSTable;
    PageAlloc::DMAAlloc::DMABuffer* DMA_ScratchpadArray; // array of U64 phys ptrs
    PageAlloc::DMAAlloc::DMABuffer* DMA_Scratchpads[64]; // cap to 64 for now
    U32 ScratchpadCount;

        volatile xHCITRB *VCmdRing;
        volatile xHCITRB *VEventRing;
        volatile U64 *V_DCBAAP;

        U32 CmdRingSize;
        U32 EventRingSize;
        BOOL EventRingCycleState;
        U32 CmdRingEnqueueIndex;
        BOOL CmdRingCycleState;
        U32 EventRingDequeueIndex;
    // Counter for spurious interrupts observed on this controller
    U32 SpuriousInterruptCount;

        // Simple state to avoid double-issuing Enable Slot on repeated PSC
        struct xHCIPortState {
            U8 State; // see enum below for symbolic names
        };
        xHCIPortState PortStates[256]; // Array state buat tiap port
        // Port state symbolic values
        enum : U8 {
            PORT_STATE_EMPTY = 0,
            PORT_STATE_CONNECTED = 1,
            PORT_STATE_RESETTING = 2,
            PORT_STATE_ENABLED = 3,
            PORT_STATE_ADDRESSING = 4,
            PORT_STATE_ENABLE_SENT = 5 // per-port: Enable Slot command already sent
        };
    };

    // TODO: Define global xHCI controller array

    extern xHCIDriver g_xhci_controllers[XHCI_MAX_CONTROLLERS];
    extern int g_xhci_controller_count;

    VOID RegisterController(U8 Bus, U8 Device, U8 Function, U8 MSICapOffset);
    VOID InitializeAllControllers();
    // Test helper: issue Enable Slot without NOOP and without polling; rely on MSI CCE
    VOID SimulateDeviceConnectTest();
    // New test: issue N Enable Slot commands (no NOOP, no polling) to verify repeated interrupts
    VOID InterruptBurstTest(U32 times);
}