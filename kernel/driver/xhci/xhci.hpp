#pragma once
#include "xhci_regs.hpp"
#include <rossys.hpp>
#include <rosval.h>
#include "xhci_regs.hpp" // Uncomment this when we need it
#include <mm.hpp>
#include "../hid/usb_hid_key.hpp"

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

        // Per-slot device state array maintained by the xHCI driver.
        // This holds runtime info about allocated slots/devices such as
        // EP0 ring pointers, enqueue indices and cycle state.
        struct XHCIEndpointState {
            PageAlloc::DMAAlloc::DMABuffer* Ring;
            U32 EnqueueIdx;
            BOOL CycleState;
        };

        struct XHCIDeviceState {
            PageAlloc::DMAAlloc::DMABuffer* EP0Ring;
            U32 EP0EnqueueIdx;
            BOOL EP0CycleState;
            U64 LastEP0DestPhys;

            U64 InputContextPhys; // Alamat fisik Input Context untuk Address Device
            PageAlloc::DMAAlloc::DMABuffer* IntBufferDMA; // DMA buffer untuk interrupt transfer

            U8 ActiveIntDCI;     // DCI untuk Interrupt IN (disimpan pas parsing)

            U8 LastKeyboardData[8]; 
            BOOL IsKeyboard; // Penanda kalau device ini keyboard
            U8 RepeatKeyScancode; // Scancode tombol yang lagi ditahan
            U32 RepeatCounter;    // Counter buat nunggu delay

            U8 BulkInDCI;       // DCI buat Baca Data
            U8 BulkOutDCI;      // DCI buat Kirim Command
            BOOL IsMassStorage; // Flag penanda

            BOOL IsMouse;        // Penanda kalau device ini mouse
            U8 LastMouseButtons; // State tombol mouse terakhir

            BOOL IsHub;          // Penanda kalau device ini hub

            U64 IntBufferPhys;   // Alamat fisik buffer data mouse
            U8* IntBufferVirt;   // Alamat virtual buffer data mouse
            U8 RootPortID;       // Port fisik di mana device nyolok (penting buat Slot Context)
            U8 PortSpeed;        // Speed ID (penting buat Slot Context)

            U16 IntMaxPacketSize;    // Untuk keperluan random (misal MSC)

            XHCIEndpointState Endpoints[32]; 

            VOLATILE BOOL TransferComplete;
            volatile BOOL PendingMSCInit;

            // future: add endpoint rings, address, config, speed, etc.
            XHCIDeviceState()
            { EP0Ring = nullptr; 
              EP0EnqueueIdx = 0; 
              EP0CycleState = TRUE;
              LastEP0DestPhys = 0; 
              Stage = STAGE_NONE;
              IsKeyboard = FALSE;
              IsMassStorage = FALSE;
              IsMouse = FALSE;
              IsHub = FALSE;
              TransferComplete = FALSE;
              PendingMSCInit = FALSE;

              // Init endpoints
                for(int i=0; i<32; i++) {
                    Endpoints[i].Ring = nullptr;
                    Endpoints[i].EnqueueIdx = 0;
                    Endpoints[i].CycleState = TRUE;
                }
            }

            VOLATILE enum DeviceStage {
                STAGE_NONE,
                STAGE_GET_DESCRIPTOR_SENT,
                STAGE_SET_CONFIG_SENT,
                STAGE_SET_PROTOCOL_SENT,
                STAGE_CONFIGURED,
                STAGE_GET_CONFIG_DESC_SENT,
                STAGE_ENDPOINT_CONFIG_SENT,
                STAGE_RUNNING
            } Stage;
        };

        static const U32 MAX_SLOTS = 256;
        XHCIDeviceState Devs[MAX_SLOTS];

        // Simple state to avoid double-issuing Enable Slot on repeated PSC
        struct xHCIPortState {
            U8 State; // see enum below for symbolic names
            U8 SlotID; // Assigned Slot ID after Enable Slot completes
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
    } ;

    // TODO: Define global xHCI controller array

    extern xHCIDriver g_xhci_controllers[XHCI_MAX_CONTROLLERS];
    extern int g_xhci_controller_count;

    VOID RegisterController(U8 Bus, U8 Device, U8 Function, U8 MSICapOffset);
    VOID InitializeAllControllers();
    // Test helper: issue Enable Slot without NOOP and without polling; rely on MSI CCE
    VOID SimulateDeviceConnectTest();
    // New test: issue N Enable Slot commands (no NOOP, no polling) to verify repeated interrupts
    VOID InterruptBurstTest(U32 times);

    VOID SetupAddressDevice(xHCIDriver &DRV, U8 SlotID, U8 RootPortID);
    VOID GetDeviceDescriptor(xHCIDriver &DRV, U8 SlotID);

    VOID ConfigureEndpoint(xHCIDriver &DRV, U8 SlotID, U8 EpAddr, U8 EpType, U16 MaxPacketSize, U32 Interval);
    VOID GetDescriptor(xHCIDriver &DRV, U8 SlotID, U8 DescType, U8 DescIndex, U16 Length, U64 BufferPhys);

    VOID QueueInterruptTransfer(xHCIDriver &DRV, U8 SlotID, U8 DCI, U64 BufferPhys, U32 Length);
    VOID QueueBulkTransfer(xHCIDriver &DRV, U8 SlotID, U8 DCI, U64 BufferPhys, U32 Length);
    VOID CheckPendingMSC(xHCIDriver &DRV);
    VOID SetBootProtocol(xHCIDriver &DRV, U8 SlotID);
}

// non namespace such as helper. no need namespace caus it only used internally
    VOID HandleIfHIDInput(xHCI::xHCIDriver::XHCIDeviceState &DevState, volatile xHCITRB *Event, U8 CCode, U8 dciSource);
    BOOL HandleIfBulkStorage(xHCI::xHCIDriver::XHCIDeviceState &DevState, volatile xHCITRB *Event, U8 CCode, U8 dciSource);
    U8 CalcDCISource(volatile xHCITRB *Event);
    VOID FindClassAndEndpoint(U32 &offset, U16 &totalLen, U8 *buffer, xHCI::xHCIDriver &DRV, U8 SlotID, BOOL &found, U8 &currentInterfaceClass);
    VOID ResetDevState(xHCI::xHCIDriver &DRV, U32 SlotID);
    VOID FreeDeviceResources(xHCI::xHCIDriver &DRV, U32 SlotID);