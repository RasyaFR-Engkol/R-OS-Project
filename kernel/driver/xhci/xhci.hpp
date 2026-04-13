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
            U8 ResetCount;
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

        U8 EnableSlotQueue[256];
        VOLATILE U8 EnableSlotQueueHead = 0;
        VOLATILE U8 EnableSlotQueueTail = 0;
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

// ==========================================
// USB CONSTANTS & MACROS
// ==========================================

// USB Descriptor Types
#define USB_DESC_TYPE_DEVICE        0x01 // Wajib ada buat parsing awal
#define USB_DESC_TYPE_CONFIG        0x02 // Buat parsing Configuration
#define USB_DESC_TYPE_INTERFACE     0x04
#define USB_DESC_TYPE_ENDPOINT      0x05
#define USB_DESC_TYPE_HID           0x21 // Descriptor khusus HID
#define USB_DESC_TYPE_HID_REPORT    0x22 // Wajib buat baca input Controller/Tablet

// USB Device Classes
#define USB_CLASS_AUDIO             0x01 // Headset/Soundcard USB
#define USB_CLASS_CDC               0x02 // Communications (Ethernet USB / Serial)
#define USB_CLASS_HID               0x03 // Mouse, Keyboard, Controller, Tablet
#define USB_CLASS_IMAGE_MTP         0x06 // PTP / MTP (Kamera Sony, HP Android)
#define USB_CLASS_MASS_STORAGE      0x08 // Flashdisk, HDD Eksternal
#define USB_CLASS_HUB               0x09 // USB Hub
#define USB_CLASS_VIDEO             0x0E // Webcam
#define USB_CLASS_VENDOR_SPECIFIC   0xFF // Device custom (Kadang HP pake ini)

// USB Endpoint Transfer Types (Attributes)
#define USB_EP_ATTR_TYPE_CONTROL    0x00
#define USB_EP_ATTR_TYPE_ISO        0x01 // Isochronous (Sering dipake Audio/Webcam)
#define USB_EP_ATTR_TYPE_BULK       0x02 // Flashdisk & MTP
#define USB_EP_ATTR_TYPE_INTERRUPT  0x03 // HID (Mouse/KB/Controller/Tablet)

// HID Subclass & Protocols
#define HID_SUBCLASS_BOOT           0x01 // Boot Device (Bisa jalan tanpa driver kompleks)
#define HID_PROTOCOL_NONE           0x00 // Controller, Gamepad, Pen Tablet
#define HID_PROTOCOL_KEYBOARD       0x01
#define HID_PROTOCOL_MOUSE          0x02

// Mass Storage Subclass & Protocol
#define MSC_SUBCLASS_SCSI           0x06
#define MSC_PROTOCOL_BULK_ONLY      0x50

// Image / MTP / PTP Subclass & Protocol
#define IMAGE_SUBCLASS_STILL        0x01 // Still Image Capture
#define IMAGE_PROTOCOL_PTP_MTP      0x01 // PTP atau MTP (Media Transfer)

// ========================
// EVENT COMPLETION CODES (CCode)
// ========================

#define CC_SUCCESS                  1
#define CC_SHORT_PACKET             13
#define CC_TRANSFER_ERROR           14
#define CC_BABBLE_DETECTED          3
#define CC_STALL_ERROR              5
#define CC_RESOURCE_ERROR           11
#define CC_BANDWIDTH_ERROR          12
#define CC_NO_SLOTS_AVAILABLE       15
#define CC_INVALID_STREAM_TYPE      16
#define CC_SLOT_NOT_ENABLED         17
#define CC_ENDPOINT_NOT_ENABLED     18
#define CC_SHORT_TRANSFER            19
#define CC_RING_UNDERRUN             20
#define CC_RING_OVERRUN              21
#define CC_VF_EVENT_RING_FULL        22
#define CC_PARAMETER_ERROR           23
#define CC_BANDWIDTH_OVERRUN         24
#define CC_CONTEXT_STATE_ERROR       25
#define CC_NO_PING_RESPONSE          26
#define CC_EVENT_LOST                27
#define CC_UNDEFINED_ERROR           28

// ========================

// ==============================
// TRB CMD TYPE
// ==============================

#define TRB_TYPE_NOOP                 23
#define TRB_TYPE_ENABLE_SLOT          9
#define TRB_TYPE_ADDRESS_DEVICE             11
#define TRB_TYPE_CONFIGURE_ENDPOINT        12
#define TRB_TYPE_EVALUATE_CONTEXT             13
#define TRB_TYPE_DISABLE_SLOT             10
#define TRB_TYPE_TRANSFER_EVENT         32

// ==============================
// EVENT TYPE
// ==============================
#define EVENT_TYPE_COMMAND_COMPLETION   33
#define EVENT_TYPE_PORT_STATUS_CHANGE   34
#define EVENT_TYPE_TRANSFER_EVENT       32  