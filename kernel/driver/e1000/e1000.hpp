#pragma once

#include <rosval.h>
#include <rossys.hpp>
#include <mm.hpp>
#include <network/netinterface.hpp>

namespace Network{
    namespace E1000{

        class E1000Driver;
        extern E1000Driver *g_E1000Instance;

        // Vendor & Device IDs
        #define INTEL_VEND     0x8086
        #define E1000_DEV      0x100E  // Qemu standard, 82540EM
        #define E1000_I217     0x153A  // Common on laptops
        #define E1000_82577LM  0x10EA  // Thinkpad specific variants often found

        // Konfigurasi Ring Size
        #define E1000_NUM_RX_DESC 32
        #define E1000_NUM_TX_DESC 8
        #define E1000_BUFF_SIZE   2048 // Cukup untuk standar Ethernet MTU 1500

        // Registers Offsets
        #define REG_CTRL        0x0000
        #define REG_STATUS      0x0008
        #define REG_EEPROM      0x0014
        #define REG_CTRL_EXT    0x0018
        #define REG_ICR         0x00C0 // Interrupt Cause Read
        #define REG_ICS         0x00C8 // Interrupt Cause Set
        #define REG_IMS         0x00D0 // Interrupt Mask Set
        #define REG_RCTL        0x0100 // Receive Control
        #define REG_TCTL        0x0400 // Transmit Control
        #define REG_RDBAL       0x2800 // RX Desc Base Low
        #define REG_RDBAH       0x2804 // RX Desc Base High
        #define REG_RDLEN       0x2808 // RX Desc Length
        #define REG_RDH         0x2810 // RX Desc Head
        #define REG_RDT         0x2818 // RX Desc Tail
        #define REG_TDBAL       0x3800 // TX Desc Base Low
        #define REG_TDBAH       0x3804 // TX Desc Base High
        #define REG_TDLEN       0x3808 // TX Desc Length
        #define REG_TDH         0x3810 // TX Desc Head
        #define REG_TDT         0x3818 // TX Desc Tail
        #define REG_MTA         0x5200 // Multicast Table Array

        // Control Bits
        #define RCTL_EN         (1 << 1)    // Receiver Enable
        #define RCTL_SBP        (1 << 2)    // Store Bad Packets
        #define RCTL_UPE        (1 << 3)    // Unicast Promiscuous Enabled
        #define RCTL_MPE        (1 << 4)    // Multicast Promiscuous Enabled
        #define RCTL_LPE        (1 << 5)    // Long Packet Enable
        #define RCTL_BAM        (1 << 15)   // Broadcast Accept Mode
        
        #define TCTL_EN         (1 << 1)    // Transmit Enable
        #define TCTL_PSP        (1 << 3)    // Pad Short Packets

        #define REG_TCTL        0x0400 // Transmit Control
        #define REG_TIPG        0x0410 // Transmit Inter-Packet Gap
        #define REG_RAL         0x5400 // Receive Address Low
        #define REG_RAH         0x5404 // Receive Address High

        struct RXDescriptor{
            VOLATILE U64 Address;
            VOLATILE U16 Length;
            VOLATILE U16 Checksum;
            VOLATILE U8 Status;
            VOLATILE U8 Errors;
            VOLATILE U16 Special;
        } PACKSTRUCT;

        struct TXDescriptor{
            VOLATILE U64 Address;
            VOLATILE U16 Length;
            VOLATILE U8 Cso;
            VOLATILE U8 Cmd;
            VOLATILE U8 Status;
            VOLATILE U8 Css;
            VOLATILE U16 Special;
        } PACKSTRUCT;

        class E1000Driver : public NetworkInterface{
            public:
                STATIC VOID RegisterDevice(U8 Bus, U8 Dev, U8 Func);

                VOID Initialize();
                BOOL SendRawPacket(U8 *Data, U32 Length) override;
                VOID HandleInterrupt();

            private:
                E1000Driver(U8 Bus, U8 Dev, U8 Func);

                U8 PCIBus, PCIDev, PCIFunc;
                UPTR MMIOBaseVirt;
                U64 MMIOBasePhys;

                U8 MacAddr[6];
                BOOL HasEEPROM;

                struct RXDescriptor *RXDescs;
                U64 RXDescPhys;
                VOID **RxBuffer;
                U64 *RXBufferPhys;
                U16 RXTail;

                struct TXDescriptor *TXDescs;
                U64 TXDescPhys;
                PageAlloc::DMAAlloc::DMABuffer* TXTrackers[E1000_NUM_TX_DESC];
                VOID **TXBuffers;
                U64 *TXBufferPhys;
                U16 TXTail;

                VOID WriteCommand(U16 Offset, U32 Value);
                U32 ReadCommand(U16 Offset);
                VOID DetectEEPROM();
                U16 ReadEEPROM(U8 Addr);
                VOID ReadMac();
                VOID InitRX();
                VOID InitTX();
                VOID DebugDumpTX();
                VOID SetupRXFilter();

                STATIC VOID InterruptHandler(VOID *Context);
        };
    }
}