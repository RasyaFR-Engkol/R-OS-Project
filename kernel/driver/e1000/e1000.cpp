#include <rossys.hpp>
#include "e1000.hpp"
#include <network/ethernet.hpp>
#include <network/swapper.hpp>
#include <network/arp.hpp>
#include <network/ipv4.hpp>
#include "network/checksum.hpp"
#include "network/netinterface.hpp"
#include <network/dhcp.hpp>
#include "port.hpp"
#include "rosval.h"
#define PRINTK_MODULE_NAME "E1000"
#include <logging.hpp>
#include <mm.hpp>
#include <string.hpp>
#include "../../dev/devicemanager.hpp"

#define ETHER_TYPE_IP   0x0800
#define ETHER_TYPE_ARP  0x0806

namespace Network{
    namespace E1000{
        using namespace Printk;

        E1000Driver *g_E1000Instance = nullptr;

        //
        // Untuk RW Command ke MMIO punya E1000
        // 

        VOID E1000Driver::WriteCommand(U16 Offset, U32 Value){
            *(VOLATILE U32*)(MMIOBaseVirt + Offset) = Value;
        }

        U32 E1000Driver::ReadCommand(U16 Offset){
            return *(VOLATILE U32*)(MMIOBaseVirt + Offset);
        }

        // 
        // Initialization
        // 

        VOID E1000Driver::RegisterDevice(U8 Bus, U8 Dev, U8 Func){
            g_E1000Instance = new E1000Driver(Bus, Dev, Func);
            g_E1000Instance->Initialize();
        }

        E1000Driver::E1000Driver(U8 Bus, U8 Device, U8 Function) 
            : PCIBus(Bus), PCIDev(Device), PCIFunc(Function){}

        VOID E1000Driver::Initialize(){
            Write(Level::LOG_INFO, "[E1000] Initializing device on PCI %02x:%02x.%u",
                PCIBus, PCIDev, PCIFunc);

            U32 PCICMD = PCI::ReadDword(PCIBus, PCIDev, PCIFunc, 0x04);
            PCI::WriteDword(PCIBus, PCIDev, PCIFunc, 0x04,  PCICMD | (1 << 2)); // acknowledge driver

            U32 Bar0 = PCI::ReadDword(PCIBus, PCIDev, PCIFunc, 0x10); // Ambil BAR0

            // Mask flag type (bit 0-3 biasanya flag)
            // Note: Di real hardware, harus cek apakah ini 64-bit BAR atau bukan.
            // Asumsi 32-bit MMIO untuk simplisitas QEMU
            MMIOBasePhys = Bar0 & 0xFFFFFFF0;

            // PENTING: Map MMIO Physical ke Virtual. 
            // Karena kamu belum kasih API generic MapMMIO, gw asumsi kamu punya cara mappingnya.
            // Disini gw pake asumsi kasar 'mmioBaseVirt = mmioBasePhys + OffsetDirectMapping'
            // atau kita pakai PageAlloc::MapPages kalau ada akses ke kernel PML4.
            // TODO: Ganti ini dengan fungsi mapping MMIO kernel kamu.

            // Jawab:
            // Kita ga punya MapMMIO. tapi punya MapPage
            SIZE_T mmioSize = PAGE_SIZE * 32;
            SIZE_T mmioPages = (mmioSize + PAGE_SIZE - 1) / PAGE_SIZE; // round up bytes -> pages

            MMIOBaseVirt = (UPTR)PageAlloc::VirtualAllocPages(mmioPages); // 2 pages demi keamanan
            if(!MMIOBaseVirt){
                Write(Printk::LOG_EMERG, "[E1000] VirtualAllocPages for MMIO Failed.\n panic pantat unik.\n");
                return;
            }
            if(!PageAlloc::MapPages(KernelPML4,
                 MMIOBasePhys,
                  MMIOBaseVirt,
                   mmioSize,
                    PAGE_PRESENT | PAGE_RW | PAGE_PCD))
                    {
                        Write(Printk::LOG_ERR, "[E1000] Paging MMIO Failed.\n");
                        return;
                    }
                
            Write(Level::LOG_DEBUG, "[E1000] mmio phys: %p, mmio virt: %p.\n", MMIOBasePhys, MMIOBaseVirt);

            DetectEEPROM();
            ReadMac();
            /*PrintMAC();*/

            SetupRXFilter();

            U8 IrqNum = PCI::EnableLegacyINTxForDevice(PCIBus, PCIDev, PCIFunc, E1000Driver::InterruptHandler);
            if(IrqNum == 0){
                Write(Printk::LOG_ERR, "[E1000] Failed to enable IRQ.\n");
                return;
            } else {
                Write(Level::LOG_INFO, "[E1000] IRQ %u enabled.\n", IrqNum);
            }

            InitRX();
            InitTX();

            WriteCommand(REG_IMS, (1 << 2) | (1 << 7)); // enable link status & rx timer

            U32 Ctrl = ReadCommand(REG_CTRL);
            WriteCommand(REG_CTRL, Ctrl | (1 << 26)); // SLU

            this->SetPacketHandler(Network::EthernetInput);

            String::Memcpy(this->Mac, this->MacAddr, 6); 

            // 2. Reset IP jadi 0
            this->IP = 0;

            // 3. Baru Gas Kirim DHCP Discover
            Write(Level::LOG_INFO, "[E1000] Starting DHCP Discovery...\n");
            Network::SendDHCPDiscover(this);

            /*this->IP = IPv4::ToU32(192, 168, 100, 2);
            this->SubnetMask = IPv4::ToU32(255, 255, 255, 0);
            this->Gateway = IPv4::ToU32(192, 168, 100, 1);

            Write(Level::LOG_INFO, "[E1000] TAP init complete.\n");

            Network::SendPing(this, this->Gateway, this->MacAddr);*/

            Write(Level::LOG_INFO, "[E1000] DHCP Success. E1000 initialized.\n");
        }

        //
        // Helper EEPROM dan MAC
        // 

        VOID E1000Driver::SetupRXFilter(){
            for (int i = 0; i < 128; i++) {
                WriteCommand(REG_MTA + (i * 4), 0);
            }

            U32 ral = (U32)MacAddr[0] |
              ((U32)MacAddr[1] << 8)  |
              ((U32)MacAddr[2] << 16) |
              ((U32)MacAddr[3] << 24);

            U32 rah = (U32)MacAddr[4] | 
              ((U32)MacAddr[5] << 8);

            rah |= (1 << 31); 

            WriteCommand(REG_RAL, ral);
            WriteCommand(REG_RAH, rah);
            
            Write(Level::LOG_DEBUG, "[E1000] RX Filter set. RAL: %x, RAH: %x\n", ral, rah);
        }

        VOID E1000Driver::DetectEEPROM(){
            WriteCommand(REG_EEPROM, 0x1);
            HasEEPROM = TRUE;
        }

        U16 E1000Driver::ReadEEPROM(U8 Addr){
            U32 Tmp = 0;
            if(HasEEPROM){
                WriteCommand(REG_EEPROM, (1) | ((U32)Addr << 8));
                while (!((Tmp = ReadCommand(REG_EEPROM)) & (1 << 4)));
            }
            return (U16)((Tmp >> 16) & 0xFFFF);
        }

        VOID E1000Driver::ReadMac(){
            U16 Temp;
            Temp = ReadEEPROM(0);
            MacAddr[0] = Temp & 0xFF;
            MacAddr[1] = Temp >> 8;
            Temp = ReadEEPROM(1);
            MacAddr[2] = Temp & 0xFF;
            MacAddr[3] = Temp >> 8;
            Temp = ReadEEPROM(2);
            MacAddr[4] = Temp & 0xFF;
            MacAddr[5] = Temp >> 8;
        }

        /*VOID E1000Driver::PrintMAC(){
            Write(Level::LOG_INFO, "[E1000] MAC Address: %02x:%02x:%02x:%02x:%02x:%02x\n",
            MacAddr[0], MacAddr[1], MacAddr[2], MacAddr[3], MacAddr[4], MacAddr[5]);
        }*/

        VOID E1000Driver::InitRX(){
            SIZE_T DescSize = sizeof(RXDescriptor) * E1000_NUM_RX_DESC;
            auto *DMADesc = PageAlloc::DMAAlloc::AllocateDMABytes(DescSize);
            if(!DMADesc){
                Write(Printk::LOG_ERR, "[E1000] DMA Fail RX DSC.\n");
                return;
            }

            RXDescs = (RXDescriptor*)DMADesc->VirtAddr;
            RXDescPhys = DMADesc->PhysAddr;

            RxBuffer = (VOID**)Kmalloc::Alloc(sizeof(VOID*) * E1000_NUM_RX_DESC);
            RXBufferPhys = (U64*)Kmalloc::Alloc(sizeof (U64) * E1000_NUM_RX_DESC);

            for(INTN i = 0; i < E1000_NUM_RX_DESC; i++){
                auto *BufInfo = PageAlloc::DMAAlloc::AllocateDMAPages(1);
                RXDescs[i].Address = (U64)BufInfo->PhysAddr;
                RXDescs[i].Status = 0;
                RxBuffer[i] = (VOID*)BufInfo->VirtAddr;
                RXBufferPhys[i] = BufInfo->PhysAddr;
            }

            WriteCommand(REG_RDBAL, (U32)(RXDescPhys & 0xFFFFFFFF));
            WriteCommand(REG_RDBAH, (U32)(RXDescPhys >> 32));
            
            WriteCommand(REG_RDLEN, E1000_NUM_RX_DESC * sizeof(RXDescriptor));
            WriteCommand(REG_RDH, 0);
            WriteCommand(REG_RDT, E1000_NUM_RX_DESC - 1); // Tail di akhir
            RXTail = 0; // Software tracker

            // Konfigurasi RCTL
            // Enable | Store Bad Packets | Broadcast Accept | Multicast Promisc | Strip CRC
            U32 rctl = RCTL_EN | RCTL_SBP | RCTL_UPE | RCTL_MPE | RCTL_LPE | RCTL_BAM | (1 << 26);
            WriteCommand(REG_RCTL, rctl);
        }

        VOID E1000Driver::InitTX(){
            SIZE_T DescSize = sizeof(TXDescriptor) * E1000_NUM_TX_DESC;
            auto *DMADesc = PageAlloc::DMAAlloc::AllocateDMABytes(DescSize);
            if(!DMADesc){
                Write(Printk::LOG_ERR, "[E1000] DMA Fail TX DSC.\n");
                return;
            }

            TXDescs = (TXDescriptor*)DMADesc->VirtAddr;
            TXDescPhys = DMADesc->PhysAddr;

            for(int i=0; i < E1000_NUM_TX_DESC; i++){
                TXTrackers[i] = nullptr; 
            }
            String::Memset(TXDescs, 0, DescSize);

            WriteCommand(REG_TDBAL, (U32)(TXDescPhys & 0xFFFFFFFF));
            WriteCommand(REG_TDBAH, (U32)(TXDescPhys >> 32));
            WriteCommand(REG_TDLEN, E1000_NUM_TX_DESC * sizeof(TXDescriptor));
            WriteCommand(REG_TDH, 0);
            WriteCommand(REG_TDT, 0);
            TXTail = 0;

            WriteCommand(REG_TIPG, 0x0060200A); // Default nilai TIPG

            U32 TCTL = TCTL_EN | TCTL_PSP | (15 << 4) | (64 << 12);
            WriteCommand(REG_TCTL, TCTL);
        }

        BOOL E1000Driver::SendRawPacket(U8 *Data, U32 Len){
            if (!MMIOBaseVirt) {
                Write(Level::LOG_ERR, "[E1000] MMIO not initialised. Drop packet.\n");
                return FALSE;
            }
            Write(Level::LOG_DEBUG, "--- PRE-SEND DIAGNOSTIC ---\n");
            DebugDumpTX(); // Cek status sebelum kirim

            TXTail = ReadCommand(REG_TDT);

            // ... (Bagian Garbage Collection TXTrackers biarkan sama) ...
            if (TXTrackers[TXTail] != nullptr) {
                if (TXDescs[TXTail].Status & 1) {
                    PageAlloc::DMAAlloc::FreeDMABuffer(TXTrackers[TXTail]);
                    TXTrackers[TXTail] = nullptr;
                } else {
                    Write(Level::LOG_WARNING, "[E1000] TX Ring Full! Dropping packet.\n");
                    return FALSE; 
                }
            }

            auto *PktBuf = PageAlloc::DMAAlloc::AllocateDMABytes(Len);
            if(!PktBuf) {
                Write(Level::LOG_ERR, "[E1000] OOM on TX Send\n");
                return FALSE;
            }

            // !!! DIAGNOSA ALAMAT FISIK !!!
            // Kalau PhysAddr = 0, itu masalah besar. 
            // Kalau PhysAddr > RAM yang dikasih ke QEMU, itu juga masalah.
            Write(Level::LOG_DEBUG, "[E1000] Payload PhysAddr: 0x%llx, Virt: 0x%llx, Len: %d\n", 
                (unsigned long long)PktBuf->PhysAddr, 
                (unsigned long long)PktBuf->VirtAddr, 
                Len);

            TXTrackers[TXTail] = PktBuf;
            String::Memcpy((VOID*)PktBuf->VirtAddr, Data, Len);

            TXDescs[TXTail].Address = PktBuf->PhysAddr;
            TXDescs[TXTail].Length = Len;
            // CMD: EOP | IFCS | RS
            TXDescs[TXTail].Cmd = (1 << 3) | (1 << 1) | (1 << 0);
            TXDescs[TXTail].Status = 0;

            // !!! DIAGNOSA DESCRIPTOR !!!
            // Kita print apa yang hardware lihat di descriptor ring
            Write(Level::LOG_DEBUG, "[E1000] Desc[%d] wrote: Addr=0x%llx Cmd=0x%x\n", 
                TXTail, 
                (unsigned long long)TXDescs[TXTail].Address, 
                TXDescs[TXTail].Cmd);

            UNUSED__ U8 OldTail = TXTail;
            TXTail = (TXTail + 1) % E1000_NUM_TX_DESC;
            
            // Kick the hardware
            WriteCommand(REG_TDT, TXTail);
            
            Write(Level::LOG_DEBUG, "--- POST-SEND DIAGNOSTIC ---\n");
            DebugDumpTX(); // Cek apakah TDT berubah dan TDH mulai ngejar

            return TRUE;
        }

        VOID E1000Driver::InterruptHandler(VOID* Ctx){
            if(!g_E1000Instance) return;
            g_E1000Instance->HandleInterrupt();
        }

        VOID E1000Driver::HandleInterrupt(){
            U32 Cause = ReadCommand(REG_ICR);

            if(Cause & (1 << 2)){
                Write(Level::LOG_CRIT, "[E1000] Link Status Changed.\n");
                // Reload CTRL register, check bit 'Link Up'
            }

            if(Cause & (1 << 7) || Cause & (1 << 6)){
                while((RXDescs[RXTail].Status & 1)){
                    U8 *Buf = (U8*)RxBuffer[RXTail];
                    U16 PktLen = RXDescs[RXTail].Length;

                    Write(Level::LOG_DEBUG, "[E1000] RX Packet, Len=%d.\n", PktLen);

                    EthernetHeader *Frame = (EthernetHeader*)Buf;

                    U16 Protocol = Network::ntohs(Frame->Type);

                    Write(Level::LOG_DEBUG, "[NET] RX Frame from %02x:%02x:%02x.. Type: 0x%04x Len: %d\n",
                        Frame->Src[0], Frame->Src[1], Frame->Src[2], 
                        Protocol, PktLen);

                    this->OnReceive(Buf, PktLen);

                    RXDescs[RXTail].Status = 0;
                    U32 OldTail = ReadCommand(REG_RDT);
                    U32 NewTail = (OldTail + 1) % E1000_NUM_RX_DESC;
                    WriteCommand(REG_RDT, NewTail);

                    RXTail = (RXTail + 1) % E1000_NUM_RX_DESC;
                }
            }
        } 


        VOID E1000Driver::DebugDumpTX() {
            U32 tctl = ReadCommand(REG_TCTL);
            U32 tdt = ReadCommand(REG_TDT);
            U32 tdh = ReadCommand(REG_TDH);
            U32 status = ReadCommand(REG_STATUS);
            
            Write(Level::LOG_DEBUG, 
                "[E1000 DEBUG] TCTL: %x | TDT: %d | TDH: %d | STATUS: %x\n", 
                tctl, tdt, tdh, status);

            // Analisis Singkat
            if (!(tctl & (1 << 1))) {
                Write(Level::LOG_CRIT, "[E1000 ALERT] TX IS DISABLED IN TCTL!\n");
            }
            if (tdt == tdh) {
                Write(Level::LOG_DEBUG, "[E1000 INFO] TX Ring Empty (Idle)\n");
            } else {
                Write(Level::LOG_DEBUG, "[E1000 INFO] Hardware is processing... (Head!=Tail)\n");
            }
        }
    }

    void TestNetwork() {
        using namespace Network::E1000;
        
        // 1. Siapkan Buffer
        // Min 64 bytes (Minimum Ethernet Frame)
        U8 PacketBuffer[64]; 
        String::Memset(PacketBuffer, 0, 64);
        
        // Casting buffer ke struct header
        EthernetHeader* frame = (EthernetHeader*)PacketBuffer;
        
        // 2. Isi Destination: Broadcast (FF:FF:FF:FF:FF:FF)
        for(int i=0; i<6; i++) frame->Dest[i] = 0xFF;
        
        // 3. Isi Source: MAC Address E1000 kita 
        // (Sebaiknya ambil dari g_E1000Instance->MacAddr nanti)
        frame->Src[0] = 0x52; frame->Src[1] = 0x54; frame->Src[2] = 0x00;
        frame->Src[3] = 0x12; frame->Src[4] = 0x34; frame->Src[5] = 0x56;
        
        // 4. EtherType: 0x1234 (Dummy Type)
        // Ingat x86 itu Little Endian, Network itu Big Endian.
        // Kalau mau tulis 0x1234 di kabel, di memori harus 0x3412 (atau pake htons)
        frame->Type = 0x3412; 
        
        // 5. Isi Payload (Disini bedanya)
        // Kita hitung offset dimana payload dimulai
        // (PacketBuffer + Ukuran Header)
        U8* PayloadPtr = PacketBuffer + sizeof(EthernetHeader);
        
        const char* msg = "Hello RasyaOS!";
        // Copy string ke posisi payload
        String::Memcpy(PayloadPtr, msg, 14);
        
        // 6. KIRIM!
        Printk::Write(Printk::Level::LOG_INFO, "[TEST] Sending Packet...\n");
        
        if (g_E1000Instance) {
            // Kirim total 64 bytes (Header + Payload + Padding/Zeroes)
            g_E1000Instance->SendRawPacket(PacketBuffer, 64);
            Printk::Write(Printk::Level::LOG_INFO, "[TEST] Packet sent to hardware ring!\n");
        } else {
            Printk::Write(Printk::Level::LOG_ERR, "[TEST] Driver not initialized!\n");
        }
    }

    VOID BroadcastARP(U32 TargetIP){
        using namespace Network::E1000;

        if(!g_E1000Instance) return;

        U8 Buffer[64];
        String::Memset(Buffer, 0, 64);

        EthernetHeader* Eth = (EthernetHeader*)Buffer;
        for(int i=0; i<6; i++) Eth->Dest[i] = 0xFF; // Broadcast
        
        U8 MyMac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
        String::Memcpy(Eth->Src, MyMac, 6);

        Eth->Type = Network::htons(ETHER_TYPE_ARP);

        ARPHeader* Arp = (ARPHeader*)(Eth + 1);
        Arp->HardwareType = Network::htons(1); // Ethernet
        Arp->ProtocolType = Network::htons(0x0800); // IPv4
        Arp->HardwareAddrLen = 6;
        Arp->ProtocolAddrLen = 4;
        Arp->OpCode = Network::htons(ARP_OP_REQUEST);

        String::Memcpy(Arp->SrcMac, MyMac, 6);

        U8 MyIPBytes[] = {10, 0, 2, 15};
        String::Memcpy(&Arp->SrcIp, MyIPBytes, 4);

        String::Memset(Arp->DestMac, 0, 6); // Unknown

        U8 TargetIPBytes[] = {10, 0, 2, 2};
        String::Memcpy(&Arp->DestIP, TargetIPBytes, 4);

        Write(Printk::Level::LOG_INFO, "[NET] Sending ARP Request for %d.%d.%d.%d\n",
            TargetIPBytes[0], TargetIPBytes[1], TargetIPBytes[2], TargetIPBytes[3]);

        g_E1000Instance->SendRawPacket(Buffer, 64);
    }
}