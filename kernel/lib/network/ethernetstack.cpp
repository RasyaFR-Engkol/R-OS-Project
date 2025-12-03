#include "rosval.h"
#include <network/netinterface.hpp>
#include <network/ethernet.hpp>
#include <network/arp.hpp>
#include <network/ipv4.hpp>
#include <network/swapper.hpp>
#include <network/checksum.hpp>
#include <network/dhcp.hpp>
#include <network/dns.hpp>
#include <network/tcp.hpp>
#include <network/udp.hpp>
#include <string.hpp>
#include <logging.hpp>
#include <mm.hpp>

namespace Network{
    struct ARPEntry{
        U32 IPAddr;
        U8 MACAddr[6];
        BOOL Valid;
    };

    PendingPacket g_PendingUDP = { FALSE, 0, 0, 0, 0, nullptr, 0 };
    UDPSocket UdpSocketTable[MAX_UDP_SOCKETS];
    ICMPSocket IcmpSocketTable[MAX_ICMP_SOCKETS];

    ARPEntry ArpCache[64];
    INTN ARPCacheIndex = 0;

    VOID HandleIP(NetworkInterface* Card, EthernetHeader* Eth);
    VOID HandleARP(NetworkInterface* Card, EthernetHeader* Eth);

    ICMPSocket *GetICMPSocket(U64 ID){
        for(int i=0; i<MAX_ICMP_SOCKETS; i++) {
            if (IcmpSocketTable[i].Allocated && IcmpSocketTable[i].ID == ID) {
                return &IcmpSocketTable[i];
            }
        }
        return nullptr;
    }

    ICMPSocket* ICMPOpen(U16 ID) {
        if (GetICMPSocket(ID)) return nullptr; // ID already used

        for(int i=0; i<MAX_ICMP_SOCKETS; i++) {
            if (!IcmpSocketTable[i].Allocated) {
                IcmpSocketTable[i].Allocated = TRUE;
                IcmpSocketTable[i].ID = ID;
                
                // Buffer 2KB cukup buat ping reply
                IcmpSocketTable[i].RecvBufferSize = 2048;
                IcmpSocketTable[i].RecvBuffer = (U8*)Kmalloc::Alloc(2048);
                IcmpSocketTable[i].RecvWritePtr = 0;
                IcmpSocketTable[i].RecvReadPtr = 0;
                
                Printk::Write(Printk::Level::LOG_INFO, "[ICMP] Socket Created for ID 0x%04x\n", ID);
                return &IcmpSocketTable[i];
            }
        }
        return nullptr;
    }

    VOID ICMPClose(ICMPSocket* Sock) {
        if (Sock && Sock->Allocated) {
            // 1. Bebaskan Buffer Memori
            if (Sock->RecvBuffer) {
                Kmalloc::Free(Sock->RecvBuffer);
            }

            // 2. Reset Status Socket
            Sock->Allocated = FALSE;
            Sock->RecvBuffer = nullptr;
            Sock->RecvBufferSize = 0;
            
            Printk::Write(Printk::Level::LOG_DEBUG, "[ICMP] Socket ID 0x%04x Closed & Freed.\n", Sock->ID);
        }
    }

    U32 ICMPRecv(ICMPSocket* Sock, U8* Buffer, U32 MaxLen) {
        if (!Sock || !Sock->Allocated) return 0;
        
        U32 BytesRead = 0;
        while (BytesRead < MaxLen && Sock->RecvReadPtr != Sock->RecvWritePtr) {
            Buffer[BytesRead] = Sock->RecvBuffer[Sock->RecvReadPtr];
            Sock->RecvReadPtr = (Sock->RecvReadPtr + 1) % Sock->RecvBufferSize;
            BytesRead++;
        }
        return BytesRead;
    }

    UDPSocket* GetUDPSocket(U16 Port) {
        for(int i=0; i<MAX_UDP_SOCKETS; i++) {
            if (UdpSocketTable[i].Allocated && UdpSocketTable[i].LocalPort == Port) {
                return &UdpSocketTable[i];
            }
        }
        return nullptr;
    }

    // [FIX] Implementasi AllocateUDPSocket
    UDPSocket* AllocateUDPSocket() {
        for(int i=0; i<MAX_UDP_SOCKETS; i++) {
            if (!UdpSocketTable[i].Allocated) {
                UdpSocketTable[i].Allocated = TRUE;
                UdpSocketTable[i].Id = i;
                
                // Init Buffer
                UdpSocketTable[i].RecvBufferSize = 4096;
                UdpSocketTable[i].RecvBuffer = (U8*)Kmalloc::Alloc(4096);
                UdpSocketTable[i].RecvReadPtr = 0;
                UdpSocketTable[i].RecvWritePtr = 0;
                
                // Default Port (Ephemeral)
                UdpSocketTable[i].LocalPort = 50000 + i; 
                UdpSocketTable[i].RemoteIP = 0;
                UdpSocketTable[i].RemotePort = 0;
                
                return &UdpSocketTable[i];
            }
        }
        return nullptr;
    }

    UDPSocket* UDPListen(U16 Port) {
        // 1. Cek apakah port sudah ada yang pakai?
        if (GetUDPSocket(Port)) return nullptr;

        // 2. Cari slot kosong
        for(int i=0; i<MAX_UDP_SOCKETS; i++) {
            if (!UdpSocketTable[i].Allocated) {
                UdpSocketTable[i].Allocated = TRUE;
                UdpSocketTable[i].LocalPort = Port;
                
                // Alokasi Buffer 4KB
                UdpSocketTable[i].RecvBufferSize = 4096;
                UdpSocketTable[i].RecvBuffer = (U8*)Kmalloc::Alloc(4096);
                UdpSocketTable[i].RecvWritePtr = 0;
                UdpSocketTable[i].RecvReadPtr = 0;
                
                Printk::Write(Printk::Level::LOG_INFO, "[UDP] Socket Created on Port %d\n", Port);
                return &UdpSocketTable[i];
            }
        }
        return nullptr; // Table penuh
    }

    U32 UDPRecv(UDPSocket* Sock, U8* Buffer, U32 MaxLen) {
        if (!Sock || !Sock->Allocated) return 0;
        
        U32 BytesRead = 0;
        while (BytesRead < MaxLen && Sock->RecvReadPtr != Sock->RecvWritePtr) {
            Buffer[BytesRead] = Sock->RecvBuffer[Sock->RecvReadPtr];
            Sock->RecvReadPtr = (Sock->RecvReadPtr + 1) % Sock->RecvBufferSize;
            BytesRead++;
        }
        return BytesRead;
    }

    VOID UDPClose(UDPSocket* Sock) {
        if (Sock && Sock->Allocated) {
            // 1. Bebaskan Buffer Memori
            if (Sock->RecvBuffer) {
                Kmalloc::Free(Sock->RecvBuffer);
            }

            // 2. Reset Status Socket
            Sock->Allocated = FALSE;
            Sock->RecvBuffer = nullptr;
            Sock->RecvBufferSize = 0;
            
            Printk::Write(Printk::Level::LOG_DEBUG, "[UDP] Socket Port %d Closed & Freed.\n", Sock->LocalPort);
        }
    }

    BOOL GetMacFromCache(U32 TargetIP, U8 *OutMac){
        // Handle Broadcast
        if (TargetIP == 0xFFFFFFFF) {
            for(int i=0; i<6; i++) OutMac[i] = 0xFF;
            return TRUE;
        }

        // Cari di Cache
        for(INTN i = 0; i < 64; i++){
            if(ArpCache[i].Valid && ArpCache[i].IPAddr == TargetIP){
                String::Memcpy(OutMac, ArpCache[i].MACAddr, 6);
                return TRUE;
            }
        }
        return FALSE;
    }

    VOID UpdateArpCache(U32 IP, U8* Mac) {
        // Cek duplicate dulu
        for(int i=0; i<64; i++) {
            if(ArpCache[i].Valid && ArpCache[i].IPAddr == IP) {
                String::Memcpy(ArpCache[i].MACAddr, Mac, 6); // Update timestamp/mac
                return;
            }
        }
        // Cari slot kosong
        for(int i=0; i<64; i++) {
            if(!ArpCache[i].Valid) {
                ArpCache[i].IPAddr = IP;
                String::Memcpy(ArpCache[i].MACAddr, Mac, 6);
                ArpCache[i].Valid = TRUE;
                Write(Printk::Level::LOG_INFO, "[ARP] Learned: %d.%d.%d.%d is at %02x:%02x:%02x...\n", 
                    IP&0xFF, (IP>>8)&0xFF, (IP>>16)&0xFF, (IP>>24)&0xFF, Mac[0], Mac[1], Mac[2]);
                return;
            }
        }
    }

    VOID EthernetInput(NetworkInterface *Card, U8 *Data, U32 Length){
        if(Length < sizeof(EthernetHeader)) {
            Printk::Write(Printk::Level::LOG_WARNING, "[NET] Ethernet Frame too short: %u bytes\n", Length);
            return;
        }

        EthernetHeader *Eth = (EthernetHeader*)Data;
        U16 Type = Network::ntohs(Eth->Type);

        Printk::Write(Printk::Level::LOG_DEBUG, "[NET] RX Ethernet Frame Type: 0x%04x, Len=%u\n", Type, Length);

        if (Type == ETHER_TYPE_IP) {
            HandleIP(Card, Eth);
        } 
        else if (Type == ETHER_TYPE_ARP) {
            HandleARP(Card, Eth);
        } else{
            Printk::Write(Printk::Level::LOG_DEBUG, "[NET] Unknown Ethernet Type: 0x%04x\n", Type);
        }
    }

    VOID HandleARP(NetworkInterface* Card, EthernetHeader* Eth){
        ARPHeader *Arp = (ARPHeader*)(Eth + 1);
        
        UpdateArpCache(Arp->SrcIp, Arp->SrcMac);

        if (g_PendingUDP.Valid) {
            // Cek apakah MAC untuk TargetIP (atau NextHop)-nya paket pending sudah tersedia?
            // Ini agak tricky di routing. 
            // Simplifikasinya: Kalau ARP yang masuk adalah Gateway, DAN paket pending butuh Gateway, kirim!
            
            U32 NextHopCheck;
            if ((g_PendingUDP.TargetIP & Card->SubnetMask) == (Card->IP & Card->SubnetMask)) 
                NextHopCheck = g_PendingUDP.TargetIP;
            else 
                NextHopCheck = Card->Gateway;

            if (NextHopCheck == Arp->SrcIp) {
                Printk::Write(Printk::Level::LOG_INFO, "[NET] Route resolved! Resending pending Packet...\n");
                
                if (g_PendingUDP.Protocol == IP_PROTOCOL_UDP) {
                    SendUDP(Card, g_PendingUDP.TargetIP, g_PendingUDP.SrcPort, 
                            g_PendingUDP.DestPort, g_PendingUDP.Payload, g_PendingUDP.PayloadLen);
                }
                else if (g_PendingUDP.Protocol == IP_PROTOCOL_ICMP) {
                    // Panggil SendICMP pakai ID/Seq yang disimpan di SrcPort/DestPort
                    SendICMP(Card, g_PendingUDP.TargetIP, g_PendingUDP.DestPort, g_PendingUDP.SrcPort, 
                             g_PendingUDP.Payload, g_PendingUDP.PayloadLen);
                }

                Kmalloc::Free(g_PendingUDP.Payload);
                g_PendingUDP.Valid = FALSE;
            }
        }

        if(ntohs(Arp->OpCode) == ARP_OP_REQUEST){
            U32 MyIP = Card->IP;
            U32 RequestedIP = Arp->DestIP;

            Printk::Write(Printk::Level::LOG_DEBUG, "[ARP DEBUG] ReqTarget: %x vs MyIP: %x\n", Arp->DestIP, MyIP);

            U32 MyIP_flipped = ((MyIP & 0xFF) << 24) | ((MyIP & 0xFF00) << 8) | 
                           ((MyIP & 0xFF0000) >> 8) | ((MyIP & 0xFF000000) >> 24);

            if (RequestedIP == MyIP || RequestedIP == MyIP_flipped) {
                // Balas! (REPLY)
                // Kita reuse buffer yang masuk untuk hemat malloc, tinggal tukar src/dst
                
                U8 ReplyBuf[sizeof(EthernetHeader) + sizeof(ARPHeader)];
                EthernetHeader* RepEth = (EthernetHeader*)ReplyBuf;
                ARPHeader* RepArp = (ARPHeader*)(RepEth + 1);

                // Isi Ethernet Header
                String::Memcpy(RepEth->Dest, Eth->Src, 6);
                String::Memcpy(RepEth->Src, Card->Mac, 6);
                RepEth->Type = Eth->Type; // 0x0806

                // Isi ARP Header
                RepArp->HardwareType = Arp->HardwareType;
                RepArp->ProtocolType = Arp->ProtocolType;
                RepArp->HardwareAddrLen = 6;
                RepArp->ProtocolAddrLen = 4;
                RepArp->OpCode = htons(ARP_OP_REPLY);

                // 1. MAC Pengirim (Kita)
                String::Memcpy(RepArp->SrcMac, Card->Mac, 6);
                
                RepArp->SrcIp = MyIP;
                
                // 3. MAC Tujuan (Si Penanya)
                String::Memcpy(RepArp->DestMac, Arp->SrcMac, 6);
                
                // 4. IP Tujuan (Si Penanya) 
                // COPY LANGSUNG AJA! Arp->SrcIp itu udah Network Order (Big Endian)
                // Jangan diapa-apain lagi.
                RepArp->DestIP = Arp->SrcIp;

                Write(Printk::Level::LOG_INFO, "[ARP] Who has %x? Tell %x. Sending Reply.\n", MyIP_flipped, Arp->SrcIp);
                Card->SendRawPacket(ReplyBuf, sizeof(ReplyBuf));
            }
        }
    }

    VOID HandleICMP(NetworkInterface *Card, IPv4Header *IP, EthernetHeader *Eth){
        INTN IHL = (IP->VersionIHL & 0x0F) * 4;
        ICMPHeader *ICMP = (ICMPHeader*)((U8*)IP + IHL);
        U32 IPDataLen = Network::ntohs(IP->TotalLength) - IHL;

        if(ICMP->Type == 8 /* Echo Request */){
            Write(Printk::Level::LOG_INFO, "[ICMP] Echo Request received. Sending Reply.\n");

            ICMP->Type = 0;
            ICMP->Checksum = 0;
            ICMP->Checksum = Network::CalculateChecksum((U8*)ICMP, IPDataLen);

            U32 TempIP = IP->SrcIP;
            IP->SrcIP = IP->DestIP;
            IP->DestIP = TempIP;

            U8 TempMac[6];
            String::Memcpy(TempMac, Eth->Src, 6);
            String::Memcpy(Eth->Src, Eth->Dest, 6);
            String::Memcpy(Eth->Src, Card->Mac, 6);
            String::Memcpy(Eth->Dest, TempMac, 6);

            IP->HeaderChecksum = 0;
            IP->HeaderChecksum = Network::CalculateChecksum((U8*)IP, (IP->VersionIHL & 0x0F) * 4);

            Card->SendRawPacket((U8*)Eth, sizeof(EthernetHeader) + Network::ntohs(IP->TotalLength));
        } else if (ICMP->Type == 0) {
            
            U16 RecvdID = ntohs(ICMP->ID);
            U16 RecvdSeq = ntohs(ICMP->Sequence);

            ICMPSocket* Sock = GetICMPSocket(RecvdID);
            if(Sock){
                U32 ICMPHeaderLen = sizeof(ICMPHeader);
                U32 PayloadLen = IPDataLen - ICMPHeaderLen;
                U8* Payload = (U8*)(ICMP + 1);

                for(U32 i=0; i<PayloadLen; i++) {
                    Sock->RecvBuffer[Sock->RecvWritePtr] = Payload[i];
                    Sock->RecvWritePtr = (Sock->RecvWritePtr + 1) % Sock->RecvBufferSize;
                    if (Sock->RecvWritePtr == Sock->RecvReadPtr) {
                        Sock->RecvReadPtr = (Sock->RecvReadPtr + 1) % Sock->RecvBufferSize;
                    }
                }
                Printk::Write(Printk::Level::LOG_DEBUG, "[ICMP] Reply ID=%x Seq=%d delivered to Socket.\n", RecvdID, RecvdSeq);
            } else {
                U8* IPBytes = (U8*)&IP->SrcIP;
                Write(Printk::Level::LOG_INFO, "[ICMP] Unhandled Reply from %d.%d.%d.%d (ID=%x Seq=%d)\n",
                    IPBytes[0], IPBytes[1], IPBytes[2], IPBytes[3], RecvdID, RecvdSeq);
            }
        } else{
            Printk::Write(Printk::Level::LOG_DEBUG, "[ICMP] Unknown ICMP Type: %d\n", ICMP->Type);
        }
    }

    VOID HandleIP(NetworkInterface* Card, EthernetHeader* Eth) {
        IPv4Header* IP = (IPv4Header*)(Eth + 1);

        UpdateArpCache(ntohl(IP->SrcIP), Eth->Src);

        // Cek versi (4)
        if ((IP->VersionIHL >> 4) != 4) {
            Printk::Write(Printk::Level::LOG_DEBUG, "[NET] Unsupported IP Version: %d\n", IP->VersionIHL >> 4);
            return;
        }

        // Cek apakah paket buat kita?
        // (Abaikan kalau bukan buat kita dan bukan broadcast)
        if (IP->DestIP != Card->IP && 
            IP->DestIP != 0xFFFFFFFF && 
            Card->IP != 0) { // Kalau IP kita 0, terima semua (promiscuous mode IP)
                Printk::Write(Printk::Level::LOG_DEBUG, "[NET] IP Packet not for us. DestIP=%x MyIP=%x\n", IP->DestIP, Card->IP);
                return;
        }

        Printk::Write(Printk::Level::LOG_DEBUG, "[NET] RX IP Packet Protocol: %d, Len=%u\n", 
            IP->Protocol, Network::ntohs(IP->TotalLength));

        if (IP->Protocol == IP_PROTOCOL_ICMP) {
            HandleICMP(Card, IP, Eth);
        }
        else if (IP->Protocol == IP_PROTOCOL_UDP) {
            int IHL = (IP->VersionIHL & 0x0F) * 4;
            UDPHeader* Udp = (UDPHeader*)((U8*)IP + IHL);
            
            U16 DestPort = ntohs(Udp->DestPort);
            U16 SrcPort  = ntohs(Udp->SrcPort);
            U16 DataLen  = ntohs(Udp->Length) - sizeof(UDPHeader);
            U8* Data     = (U8*)(Udp + 1);

            if(DestPort == 68 /* DHCP & DNS jalan */){
                Network::HandleDHCP(Card, Data, DataLen);
                return;
            }
            if(SrcPort == 53 /* DNS Response */){
                Printk::Write(Printk::Level::LOG_DEBUG, "[DNS] Got Reply! Len=%d\n", DataLen);
                Network::HandleDNS(Card, Data, DataLen);
                return;
            }

            UDPSocket* Sock = GetUDPSocket(DestPort);
            if (Sock) {

                Sock->LastPacketInfo.SrcIP = IP->SrcIP;
                Sock->LastPacketInfo.SrcPort = SrcPort;
                Sock->LastPacketInfo.Length = DataLen;

                // Masukkan data ke Ring Buffer Socket
                for(U32 i=0; i<DataLen; i++) {
                    Sock->RecvBuffer[Sock->RecvWritePtr] = Data[i];
                    Sock->RecvWritePtr = (Sock->RecvWritePtr + 1) % Sock->RecvBufferSize;
                    
                    // Kalau buffer penuh, overwrite data lama (atau drop, terserah kebijakan)
                    if (Sock->RecvWritePtr == Sock->RecvReadPtr) {
                        Sock->RecvReadPtr = (Sock->RecvReadPtr + 1) % Sock->RecvBufferSize; // Drop old data
                    }
                }
                Printk::Write(Printk::Level::LOG_DEBUG, "[UDP] Delivered %d bytes to Socket Port %d\n", DataLen, DestPort);
                return;
            }

            char SafeBuf[64];
            int CopyLen = (DataLen > 63) ? 63 : DataLen;
            String::Memcpy(SafeBuf, Data, CopyLen);
            SafeBuf[CopyLen] = 0;
            Write(Printk::Level::LOG_INFO, "[UDP] Unhandled Packet %d -> %d: %s\n", SrcPort, DestPort, SafeBuf);
        } else if (IP->Protocol == IP_PROTOCOL_TCP) {
            // Hitung offset header IP
            int IHL = (IP->VersionIHL & 0x0F) * 4;
            // Data TCP mulai setelah IP Header
            U8* TcpData = ((U8*)IP) + IHL;
            U32 TcpLen = ntohs(IP->TotalLength) - IHL;
            
            Network::HandleTCP(Card, TcpData, TcpLen, IP->SrcIP);
        }
    }
    VOID SendPing(NetworkInterface *Card, U32 TargetIP, U8 *TargetMac) {
        // Data Ping (Payload)
        const char* PingData = "Ping Manual Bypass ARP";
        U16 PayloadLen = String::Strlen(PingData);

        // Hitung Total Panjang (Ethernet + IP + ICMP + Data)
        U32 TotalLen = sizeof(EthernetHeader) + sizeof(IPv4Header) + sizeof(ICMPHeader) + PayloadLen;
        
        // Alokasi Buffer
        U8* Buffer = (U8*)Kmalloc::Alloc(TotalLen);
        String::Memset(Buffer, 0, TotalLen);

        // --- 1. POINTER MAPPING ---
        EthernetHeader* Eth = (EthernetHeader*)Buffer;
        IPv4Header* IP = (IPv4Header*)(Eth + 1);
        ICMPHeader* Icmp = (ICMPHeader*)(IP + 1);
        U8* DataPtr = (U8*)(Icmp + 1);

        // --- 2. ISI DATA PAYLOAD ---
        String::Memcpy(DataPtr, (U8*)PingData, PayloadLen);

        // --- 3. ISI ICMP HEADER ---
        Icmp->Type = 8; // ECHO REQUEST
        Icmp->Code = 0;
        Icmp->ID = htons(0x1234);
        Icmp->Sequence = htons(1);
        Icmp->Checksum = 0;
        
        // Hitung Checksum ICMP (Header + Data)
        // Note: CalculateChecksum butuh pointer start dan panjang
        Icmp->Checksum = CalculateChecksum(Icmp, sizeof(ICMPHeader) + PayloadLen);

        // --- 4. ISI IP HEADER ---
        IP->VersionIHL = (4 << 4) | 5;
        IP->TotalLength = htons(sizeof(IPv4Header) + sizeof(ICMPHeader) + PayloadLen);
        IP->ID = htons(0);
        IP->FlagsFragment = htons(0x4000); // Don't Fragment
        IP->TTL = 64;
        IP->Protocol = IP_PROTOCOL_ICMP; // 1
        IP->SrcIP = Card->IP;
        IP->DestIP = TargetIP;
        IP->HeaderChecksum = 0;
        IP->HeaderChecksum = CalculateChecksum(IP, sizeof(IPv4Header));

        // --- 5. ISI ETHERNET HEADER (BAGIAN PENTINGNYA) ---
        // Di sini kita pake TargetMac yang lo passing di argumen!
        String::Memcpy(Eth->Dest, TargetMac, 6);
        String::Memcpy(Eth->Src, Card->Mac, 6);
        Eth->Type = htons(ETHER_TYPE_IP);

        // --- 6. KIRIM! ---
        Printk::Write(Printk::Level::LOG_INFO, "[PING] Sending RAW Ping to %x (Bypassing ARP)...\n", TargetIP);
        Card->SendRawPacket(Buffer, TotalLen);

        Kmalloc::Free(Buffer);
    }


    BOOL SendUDP(NetworkInterface* Card, U32 DestIP, U16 SrcPort, U16 DestPort, U8* Data, U16 Len) {
        
        U32 NextHopIP;
        U8 DestMac[6];

        // --- LOGIC ROUTING ---
        // 1. Cek apakah Broadcast?
        if (DestIP == 0xFFFFFFFF) {
            NextHopIP = DestIP; // Broadcast gak perlu routing
        }
        // 2. Cek apakah satu Subnet?
        // Rumus: (TargetIP & Mask) == (MyIP & Mask)
        else if ((DestIP & Card->SubnetMask) == (Card->IP & Card->SubnetMask)) {
            // Satu komplek. Kirim langsung ke orangnya.
            NextHopIP = DestIP;
        }
        // 3. Beda Subnet (Internet / Luar Kota)
        else {
            // Titip ke Pak RT (Gateway)
            // TAPI DestIP di IP Header tetap Alamat Asli (Google), cuma MAC-nya pake MAC Router.
            NextHopIP = Card->Gateway;
        }
        // ---------------------

        // Cek Broadcast MAC
        if (NextHopIP == 0xFFFFFFFF) {
            for(int i=0; i<6; i++) DestMac[i] = 0xFF;
        }
        // Cek ARP untuk Next Hop (Bisa IP Tujuan, Bisa IP Gateway)
        else if (!GetMacFromCache(NextHopIP, DestMac)) {
            Printk::Write(Printk::Level::LOG_WARNING, "[NET] Routing: Need MAC for %x. Queueing...\n", NextHopIP);
            
            // Queueing Logic (SAMA, cuma ganti target ARP)
            if(g_PendingUDP.Valid) Kmalloc::Free(g_PendingUDP.Payload);
            g_PendingUDP.TargetIP = DestIP; // Simpan tujuan asli buat retry
            g_PendingUDP.Protocol = IP_PROTOCOL_UDP;
            g_PendingUDP.SrcPort = SrcPort;
            g_PendingUDP.DestPort = DestPort;
            g_PendingUDP.PayloadLen = Len;
            g_PendingUDP.Payload = (U8*)Kmalloc::Alloc(Len);
            String::Memcpy(g_PendingUDP.Payload, Data, Len);    
            g_PendingUDP.Valid = TRUE;

            // KIRIM ARP KE NEXT HOP (Bukan ke DestIP kalau beda subnet)
            SendARPRequest(Card, NextHopIP); 
            return FALSE;
        }

        // 2. Alokasi Buffer (Eth + IP + UDP + Data)
        U32 TotalLen = sizeof(EthernetHeader) + sizeof(IPv4Header) + sizeof(UDPHeader) + Len;
        U8* Buffer = (U8*)Kmalloc::Alloc(TotalLen); // Atau PageAlloc DMA kalau driver butuh phys continuous
        
        // Pointers
        EthernetHeader* Eth = (EthernetHeader*)Buffer;
        IPv4Header* IP      = (IPv4Header*)(Eth + 1);
        UDPHeader* Udp      = (UDPHeader*)(IP + 1);
        U8* Payload         = (U8*)(Udp + 1);

        // 3. Isi Data
        String::Memcpy(Payload, Data, Len);

        // 4. Isi UDP
        Udp->SrcPort = htons(SrcPort);
        Udp->DestPort = htons(DestPort);
        Udp->Length = htons(sizeof(UDPHeader) + Len);
        Udp->Checksum = 0; // Optional di IPv4

        // 5. Isi IP
        IP->VersionIHL = (4 << 4) | 5; // Ver 4, Header 20 bytes (5 dwords)
        IP->TOS = 0;
        IP->TotalLength = htons(sizeof(IPv4Header) + sizeof(UDPHeader) + Len);
        IP->ID = htons(0); // TODO: Increment ID global
        IP->FlagsFragment = htons(0x4000); // Don't fragment
        IP->TTL = 64;
        IP->Protocol = IP_PROTOCOL_UDP;
        IP->SrcIP = Card->IP;
        IP->DestIP = DestIP;
        IP->HeaderChecksum = 0;
        IP->HeaderChecksum = CalculateChecksum(IP, sizeof(IPv4Header));

        // 6. Isi Ethernet
        String::Memcpy(Eth->Dest, DestMac, 6);
        String::Memcpy(Eth->Src, Card->Mac, 6);
        Eth->Type = htons(ETHER_TYPE_IP);

        // 7. Kirim
        Card->SendRawPacket(Buffer, TotalLen);
        
        Kmalloc::Free(Buffer);
        return TRUE;
    }

    VOID SendARPRequest(NetworkInterface* Card, U32 TargetIP) {
        U8 Buffer[64]; // Min Eth frame
        String::Memset(Buffer, 0, 64);
        
        EthernetHeader* Eth = (EthernetHeader*)Buffer;
        ARPHeader* Arp = (ARPHeader*)(Eth + 1);

        // Broadcast MAC FF:FF:FF:FF:FF:FF
        for(int i=0; i<6; i++) Eth->Dest[i] = 0xFF;
        String::Memcpy(Eth->Src, Card->Mac, 6);
        Eth->Type = htons(ETHER_TYPE_ARP);

        Arp->HardwareType = htons(1);
        Arp->ProtocolType = htons(0x0800);
        Arp->HardwareAddrLen = 6;
        Arp->ProtocolAddrLen = 4;
        Arp->OpCode = htons(ARP_OP_REQUEST);
        
        String::Memcpy(Arp->SrcMac, Card->Mac, 6);
        Arp->SrcIp = Card->IP;
        
        String::Memset(Arp->DestMac, 0, 6); // Unknown
        Arp->DestIP = TargetIP;

        Card->SendRawPacket(Buffer, 64);
    }

    BOOL SendICMP(NetworkInterface* Card, U32 DestIP, U16 Seq, U16 ID, U8* Data, U16 Len){
        U32 NextHopIP;
        U8 DestMac[6];

        if (DestIP == 0xFFFFFFFF) NextHopIP = DestIP;
        else if ((DestIP & Card->SubnetMask) == (Card->IP & Card->SubnetMask)) NextHopIP = DestIP;
        else NextHopIP = Card->Gateway; // <--- ROUTING JALAN DISINI

        if (!GetMacFromCache(NextHopIP, DestMac)) {
            Printk::Write(Printk::Level::LOG_WARNING, "[NET] Routing: Need MAC for %x. Queueing...\n", NextHopIP);
            
            if(g_PendingUDP.Valid) Kmalloc::Free(g_PendingUDP.Payload);
            
            g_PendingUDP.Valid = TRUE;
            g_PendingUDP.Protocol = IP_PROTOCOL_ICMP; // <--- TANDAI INI ICMP
            g_PendingUDP.TargetIP = DestIP; 
            
            // SIMPAN ID & SEQ (Penting biar reply-nya match)
            g_PendingUDP.SrcPort = ID;  
            g_PendingUDP.DestPort = Seq; 
            
            g_PendingUDP.PayloadLen = Len;
            g_PendingUDP.Payload = (U8*)Kmalloc::Alloc(Len);
            String::Memcpy(g_PendingUDP.Payload, Data, Len);    

            SendARPRequest(Card, NextHopIP); 
            return FALSE;
        }

        U32 TotalLen = sizeof(EthernetHeader) + sizeof(IPv4Header) + sizeof(ICMPHeader) + Len;
        U8* Buffer = (U8*)Kmalloc::Alloc(TotalLen);

        EthernetHeader* Eth = (EthernetHeader*)Buffer;
        IPv4Header* IP      = (IPv4Header*)(Eth + 1);
        ICMPHeader* Icmp    = (ICMPHeader*)(IP + 1);
        U8* Payload         = (U8*)(Icmp + 1);

        String::Memcpy(Payload, Data, Len);

        // Isi ICMP Header (Type 8 = Echo Request)
        Icmp->Type = 8; 
        Icmp->Code = 0;
        Icmp->ID = htons(ID);
        Icmp->Sequence = htons(Seq);
        Icmp->Checksum = 0;
        Icmp->Checksum = CalculateChecksum(Icmp, sizeof(ICMPHeader) + Len);

        // Isi Ethernet Header
        IP->VersionIHL = (4 << 4) | 5;
        IP->TOS = 0;
        IP->TotalLength = htons(sizeof(IPv4Header) + sizeof(ICMPHeader) + Len);
        IP->ID = htons(0);
        IP->FlagsFragment = htons(0x4000);
        IP->TTL = 64;
        IP->Protocol = IP_PROTOCOL_ICMP; // <--- PENTING
        IP->SrcIP = Card->IP;
        IP->DestIP = DestIP;
        IP->HeaderChecksum = 0;
        IP->HeaderChecksum = CalculateChecksum(IP, sizeof(IPv4Header));
        String::Memcpy(Eth->Dest, DestMac, 6);
        String::Memcpy(Eth->Src, Card->Mac, 6);
        Eth->Type = htons(ETHER_TYPE_IP);

        Card->SendRawPacket(Buffer, TotalLen);
        Kmalloc::Free(Buffer);

        return TRUE;
    }

    /*BOOL GetMacForIP(NetworkInterface *Card, U32 TargetIP, U8 *OutMac){
        for(INTN i = 0; i < 64; i++){
            if(ARPTable[i].Valid && ARPTable[i].IPAddr == TargetIP){
                String::Memcpy(OutMac, ARPTable[i].MACAddr, 6);
                return TRUE;
            }
        }
        return FALSE; // Not found
    }*/
}