#include "network/arp.hpp"
#include "network/ethernet.hpp"
#include "rossys.hpp"
#include <network/tcp.hpp>
#include <network/netinterface.hpp>
#include <network/ipv4.hpp>
#include <network/swapper.hpp>
#include <network/checksum.hpp> // Pastikan ada PseudoHeader struct di sini
#include <../kernel/driver/e1000/e1000.hpp>
#include <logging.hpp>
#include <mm.hpp>
#include <string.hpp>
#include <rng/entrophy.hpp>

#include <rostime.hpp> // Butuh Time::GetTick()

namespace Network{
    TCPSocket SocketTable[MAX_TCP_SOCKETS];

    TCPSocket *AllocateSocket(){
        for(int i=0; i<MAX_TCP_SOCKETS; i++) {
            if (!SocketTable[i].Allocated) {
                SocketTable[i].Allocated = TRUE;
                SocketTable[i].ID = i;
                SocketTable[i].State = CLOSED;
                
                // Init Retransmission & Buffer
                SocketTable[i].WaitingAck = FALSE;
                SocketTable[i].LastPacket = nullptr;
                SocketTable[i].RecvBufferSize = 8192; // 8KB Buffer
                SocketTable[i].RecvBuffer = (U8*)Kmalloc::Alloc(SocketTable[i].RecvBufferSize);
                SocketTable[i].RecvWritePtr = 0;
                SocketTable[i].RecvReadPtr = 0;
                
                SocketTable[i].Domain = (char*)Kmalloc::Alloc(256);
                if(SocketTable[i].Domain) SocketTable[i].Domain[0] = 0;

                return &SocketTable[i];
            }
        }
        return nullptr; // Penuh
    }

    VOID FreeSocket(TCPSocket *Sock){
        if(Sock) {
            Sock->Allocated = FALSE;
            Sock->State = CLOSED;
            
            if(Sock->LastPacket) {
                Kmalloc::Free(Sock->LastPacket);
                Sock->LastPacket = nullptr;
            }
            if(Sock->RecvBuffer) {
                Kmalloc::Free(Sock->RecvBuffer);
                Sock->RecvBuffer = nullptr;
            }
            if(Sock->Domain) {
                Kmalloc::Free(Sock->Domain);
                Sock->Domain = nullptr;
            }

            TCPSegmentNode* Node = Sock->OOOList;
            while(Node != nullptr) {
                TCPSegmentNode* Next = Node->Next;
                Kmalloc::Free(Node->Payload);
                Kmalloc::Free(Node);
                Node = Next;
            }
            Sock->OOOList = nullptr;
        }
    }

    TCPSocket *GetSocket(U16 LocalPort, U32 RemoteIP, U16 RemotePort){
        for(int i=0; i<MAX_TCP_SOCKETS; i++) {
            if (SocketTable[i].Allocated && SocketTable[i].LocalPort == LocalPort) {
                // Debugging: Print kalau Port Lokal cocok tapi IP/RemotePort gak cocok
                // Ignore kalau socket adalah Listener (RemoteIP == 0)
                if ((SocketTable[i].RemoteIP != RemoteIP || SocketTable[i].RemotePort != RemotePort) && SocketTable[i].RemoteIP != 0) {
                     U8* TableIP = (U8*)&SocketTable[i].RemoteIP;
                     U8* SearchIP = (U8*)&RemoteIP;
                     
                     Printk::Write(Printk::Level::LOG_DEBUG, 
                         "[TCP DEBUG] Sock mismatch! Looking for IP:%d.%d.%d.%d Port:%d | Table has IP:%d.%d.%d.%d Port:%d\n",
                         SearchIP[0], SearchIP[1], SearchIP[2], SearchIP[3], RemotePort,
                         TableIP[0], TableIP[1], TableIP[2], TableIP[3], SocketTable[i].RemotePort);
                }
            }

            if (SocketTable[i].Allocated &&
                SocketTable[i].LocalPort == LocalPort &&
                SocketTable[i].RemoteIP == RemoteIP &&
                SocketTable[i].RemotePort == RemotePort) 
            {
                return &SocketTable[i];
            }
        }
        return nullptr;
    }

    TCPSocket *GetListener(U16 LocalPort){
        for(int i=0; i<MAX_TCP_SOCKETS; i++) {
            if (SocketTable[i].Allocated &&
                SocketTable[i].State == LISTEN &&
                SocketTable[i].LocalPort == LocalPort)
            {
                return &SocketTable[i];
            }
        }
        return nullptr;
    }

    VOID AddOOOSegment(TCPSocket* Sock, U32 Seq, U8* Data, U32 Len) {
        // 1. Buat Node Baru
        TCPSegmentNode* NewNode = (TCPSegmentNode*)Kmalloc::Alloc(sizeof(TCPSegmentNode));
        NewNode->Sequence = Seq;
        NewNode->Length = Len;
        NewNode->Payload = (U8*)Kmalloc::Alloc(Len);
        if (!NewNode->Payload) {
            Kmalloc::Free(NewNode);
            return; // Gagal alokasi payload
        }
        String::Memcpy(NewNode->Payload, Data, Len);
        NewNode->Next = nullptr;

        Printk::Write(Printk::Level::LOG_DEBUG, "[TCP OOO] Storing Seq %u (Len %u)\n", Seq, Len);

        // 2. Masukkan ke Linked List (Urutkan dari kecil ke besar)
        if (Sock->OOOList == nullptr || Seq < Sock->OOOList->Sequence) {
            // Masukkan di depan (Head)
            NewNode->Next = Sock->OOOList;
            Sock->OOOList = NewNode;
        } else {
            // Cari posisi di tengah/belakang
            TCPSegmentNode* Current = Sock->OOOList;
            while (Current->Next != nullptr && Current->Next->Sequence < Seq) {
                Current = Current->Next;
            }
            
            // Cek duplikat
            if (Current->Sequence == Seq || (Current->Next && Current->Next->Sequence == Seq)) {
                // Duplikat, buang aja
                Kmalloc::Free(NewNode->Payload);
                Kmalloc::Free(NewNode);
                return;
            }

            NewNode->Next = Current->Next;
            Current->Next = NewNode;
        }
    }

    // Helper: Cek apakah paket di OOO List sudah bisa diproses?
    VOID ProcessOOOList(TCPSocket* Sock) {
        while (Sock->OOOList != nullptr) {
            TCPSegmentNode* Head = Sock->OOOList;

            // Kalau Seq di Head == TargetSequence kita, berarti gap sudah terisi!
            if (Head->Sequence == Sock->TargetSequence) {
                Printk::Write(Printk::Level::LOG_DEBUG, "[TCP OOO] Gap filled! Processing Seq %u\n", Head->Sequence);

                // Masukkan ke Ring Buffer Socket
                for(U32 i=0; i<Head->Length; i++) {
                    Sock->RecvBuffer[Sock->RecvWritePtr] = Head->Payload[i];
                    Sock->RecvWritePtr = (Sock->RecvWritePtr + 1) % Sock->RecvBufferSize;
                    if (Sock->RecvWritePtr == Sock->RecvReadPtr) {
                        Sock->RecvReadPtr = (Sock->RecvReadPtr + 1) % Sock->RecvBufferSize;
                    }
                }

                // Update Sequence
                Sock->TargetSequence += Head->Length;

                // Hapus Node & Maju ke Next
                Sock->OOOList = Head->Next;
                Kmalloc::Free(Head->Payload);
                Kmalloc::Free(Head);
            } 
            else if (Head->Sequence < Sock->TargetSequence) {
                // Kasus aneh: Paket di list ternyata udah basi (Old). Hapus aja.
                Sock->OOOList = Head->Next;
                Kmalloc::Free(Head->Payload);
                Kmalloc::Free(Head);
            }
            else {
                // Head->Sequence > TargetSequence. Masih bolong. Berhenti.
                break;
            }
        }
    }

    VOID SendTCPRaw(NetworkInterface* Card, TCPSocket *Sock, U8 Flags, U8 *Payload, U16 PayloadLen){
        // 1. Alokasi Memori buat Paket Lengkap
        U32 TotalLen = sizeof(EthernetHeader) + sizeof(IPv4Header) + sizeof(TCPHeader) + PayloadLen;
        U8 *Buffer = (U8*)Kmalloc::Alloc(TotalLen);
        String::Memset(Buffer, 0, TotalLen);

        // 2. Mapping Pointer biar gampang ngisinya
        EthernetHeader *Eth = (EthernetHeader*)Buffer;
        IPv4Header *IP = (IPv4Header*)(Eth + 1);
        TCPHeader *Tcp = (TCPHeader*)(IP + 1);
        U8 *DataPtr = (U8*)(Tcp + 1);

        // 3. Copy Data (Kalau ada payload, misal HTTP request)
        if (PayloadLen > 0 && Payload != nullptr) {
            String::Memcpy(DataPtr, Payload, PayloadLen);
        }

        // 4. ISI TCP HEADER
        Tcp->SrcPort = htons(Sock->LocalPort);
        Tcp->DestPort = htons(Sock->RemotePort);
        Tcp->SequenceNum = htonl(Sock->MySequence);
        Tcp->AckNum = htonl(Sock->TargetSequence);
        Tcp->DataOffset = (5 << 4); // Header Length 20 bytes

        U32 UsedBuffer = 0;
        if (Sock->RecvWritePtr >= Sock->RecvReadPtr) {
            UsedBuffer = Sock->RecvWritePtr - Sock->RecvReadPtr;
        } else {
            UsedBuffer = Sock->RecvBufferSize - (Sock->RecvReadPtr - Sock->RecvWritePtr);
        }

        U16 FreeSpace = (U16)(Sock->RecvBufferSize - UsedBuffer);

        if (FreeSpace < 1460) { 
            FreeSpace = 0; 
        }

        Tcp->Flags = Flags;
        Tcp->WindowSize = htons(FreeSpace);
        Tcp->UrgentPtr = 0;
        Tcp->Checksum = 0;

        // Hitung Checksum TCP (Pake Pseudo Header)
        PseudoHeader Pseudo = {};
        Pseudo.SrcIP = Sock->LocalIP;   // Asumsi Sock->LocalIP udah Network Order (dari Card->IP)
        Pseudo.DestIP = Sock->RemoteIP;
        Pseudo.Reserved = 0;
        Pseudo.Protocol = IP_PROTOCOL_TCP;
        Pseudo.Length = htons(sizeof(TCPHeader) + PayloadLen);

        // Trik hitung checksum buffer gabungan
        U32 CsumLen = sizeof(PseudoHeader) + sizeof(TCPHeader) + PayloadLen;
        U8* CsumBuf = (U8*)Kmalloc::Alloc(CsumLen);
        String::Memcpy(CsumBuf, &Pseudo, sizeof(PseudoHeader));
        String::Memcpy(CsumBuf + sizeof(PseudoHeader), Tcp, sizeof(TCPHeader) + PayloadLen);
        
        Tcp->Checksum = CalculateChecksum(CsumBuf, CsumLen);
        Kmalloc::Free(CsumBuf);

        // 5. ISI IP HEADER
        IP->VersionIHL = (4 << 4) | 5;
        IP->TotalLength = htons(sizeof(IPv4Header) + sizeof(TCPHeader) + PayloadLen);
        IP->ID = htons(0);
        IP->FlagsFragment = htons(0x4000);
        IP->TTL = 64;
        IP->Protocol = IP_PROTOCOL_TCP;
        IP->SrcIP = Card->IP;      
        IP->DestIP = Sock->RemoteIP;
        IP->HeaderChecksum = 0;
        IP->HeaderChecksum = CalculateChecksum(IP, sizeof(IPv4Header));

        // 6. ROUTING (Cari Next Hop)
        U32 NextHop = Sock->RemoteIP;

        // Cek Subnet (Card->IP dan Mask biasanya formatnya sama kayak RemoteIP secara bitwise)
        if ((NextHop & Card->SubnetMask) != (Card->IP & Card->SubnetMask)) {
             // Kalau beda subnet, NextHop jadi Gateway (Network Order / 0xC0...)
             NextHop = Card->Gateway;
        }

        // 7. ISI ETHERNET HEADER
        U8 DestMac[6];
        
        // PENTING: Cek apakah MAC address ketemu?
        if (!GetMacFromCache(NextHop, DestMac)) {
            Printk::Write(Printk::Level::LOG_WARNING, "[TCP] MAC not found for %x. Dropping packet.\n", NextHop);
    
            // Queueing Logic (SAMA, cuma ganti target ARP)
            if(g_PendingUDP.Valid) Kmalloc::Free(g_PendingUDP.Payload);
            g_PendingUDP.TargetIP = 0;
            g_PendingUDP.Protocol = IP_PROTOCOL_UDP;
            g_PendingUDP.SrcPort = Tcp->SrcPort;
            g_PendingUDP.DestPort = Tcp->DestPort;
            g_PendingUDP.PayloadLen = PayloadLen;
            g_PendingUDP.Payload = (U8*)Kmalloc::Alloc(PayloadLen);
            String::Memcpy(g_PendingUDP.Payload, Payload, PayloadLen);    
            g_PendingUDP.Valid = TRUE;

            // KIRIM ARP KE NEXT HOP (Bukan ke DestIP kalau beda subnet)
            SendARPRequest(Card, NextHop); 
            return;
        }

        EthernetHeader* EthHeader = (EthernetHeader*)Buffer;
        String::Memcpy(EthHeader->Dest, DestMac, 6);
        String::Memcpy(EthHeader->Src, Card->Mac, 6);
        EthHeader->Type = htons(ETHER_TYPE_IP);
        
        // 8. FINAL: TENDANG KE KABEL!
        Card->SendRawPacket(Buffer, TotalLen);
        
        // --- RETRANSMISSION LOGIC ---
        // Kalau paket ini mengandung SYN, FIN, atau Payload Data, kita harus simpan copy-nya
        // untuk jaga-jaga kalau hilang. (ACK murni gak perlu diretransmisi)
        if ((Flags & (TCP_SYN | TCP_FIN)) || PayloadLen > 0) {
            if (Sock->LastPacket) Kmalloc::Free(Sock->LastPacket);
            
            // Simpan Copy Paket (Hanya Payload + Flags info, bukan full Ethernet frame)
            // Simplifikasi: Kita simpan Payload mentah dan Flags. Header dibikin ulang nanti.
            Sock->LastPacket = (U8*)Kmalloc::Alloc(PayloadLen);
            if (PayloadLen > 0) String::Memcpy(Sock->LastPacket, Payload, PayloadLen);
            
            Sock->LastPacketLen = PayloadLen;
            Sock->LastFlags = Flags;
            Sock->LastSendTime = Arch::Time::GetTickCount(); // Catat waktu kirim
            Sock->WaitingAck = TRUE;
        }

        // 9. BERSIH-BERSIH
        Kmalloc::Free(Buffer);
    }

    // Fungsi baru untuk cek timeout (Panggil di timer interrupt, misal tiap 100ms)
    VOID TCPCheckRetransmission(NetworkInterface* Card) {
        U32 Now = Arch::Time::GetTickCount();
        U32 Timeout = 1000; // 1 Detik (asumsi 1 tick = 1 ms, sesuaikan dengan OS mu)

        for(int i=0; i<MAX_TCP_SOCKETS; i++) {
            TCPSocket* Sock = &SocketTable[i];
            if (Sock->Allocated && Sock->WaitingAck) {
                if (Now - Sock->LastSendTime > Timeout) {
                    Printk::Write(Printk::Level::LOG_WARNING, "[TCP] Retransmission Timeout on Sock %d! Resending...\n", Sock->ID);
                    
                    // Kirim Ulang
                    // Note: Sequence Number JANGAN di-increment lagi! Pake yang lama.
                    // Tapi SendTCPRaw kita pake Sock->MySequence.
                    // Masalah: Sock->MySequence udah maju duluan di SendTCPData.
                    // Solusi Hacky: Mundurin dulu Seq-nya sementara.
                    
                    U32 CurrentSeq = Sock->MySequence;
                    
                    // Hitung mundur: Kalau SYN/FIN makan 1 seq. Data makan Len seq.
                    U32 SeqLen = Sock->LastPacketLen;
                    if (Sock->LastFlags & (TCP_SYN | TCP_FIN)) SeqLen++;
                    
                    Sock->MySequence -= SeqLen; 
                    
                    SendTCPRaw(Card, Sock, Sock->LastFlags, Sock->LastPacket, Sock->LastPacketLen);
                    
                    Sock->MySequence = CurrentSeq; // Balikin lagi
                    
                    Sock->LastSendTime = Now; // Reset timer
                }
            }
        }
    }

    VOID SendTCPData(NetworkInterface* Card, TCPSocket *Sock, U8* Data, U16 Len) {
        if (!Sock || Sock->State != ESTABLISHED) {
             Printk::Write(Printk::Level::LOG_WARNING, "[TCP] Socket invalid or not connected!\n");
             return;
        }

        Printk::Write(Printk::Level::LOG_INFO, "[TCP] Sending %d bytes data...\n", Len);

        // Kirim paket dengan Flag PSH (Push) dan ACK
        SendTCPRaw(Card, Sock, TCP_PSH | TCP_ACK, Data, Len);

        // PENTING: Update Sequence Number kita!
        // Di TCP, Sequence nambah sesuai jumlah byte yang dikirim.
        Sock->MySequence += Len;
    }

    VOID TCPListen(NetworkInterface* Card, U16 LocalPort){
        TCPSocket* Sock = AllocateSocket();
        if (!Sock) {
            Printk::Write(Printk::Level::LOG_ERR, "[TCP] Socket Table Full!\n");
            return;
        }

        Sock->State = LISTEN;
        Sock->LocalIP = Card->IP;
        Sock->LocalPort = LocalPort;
        Sock->RemoteIP = 0;   // Terima siapa aja
        Sock->RemotePort = 0; // Terima siapa aja
        
        Printk::Write(Printk::Level::LOG_INFO, "[TCP] Socket %d Listening on %d\n", Sock->ID, LocalPort);
    }

    TCPSocket *TCPConnect(NetworkInterface *Card, U32 TargetIP, U16 TargetPort, const char* Domain){
        TCPSocket *Sock = AllocateSocket();
        if(!Sock) return nullptr;

        Sock->State = CLOSED;
        Sock->LocalIP = Card->IP;
        Sock->RemoteIP = TargetIP;
        Sock->LocalPort = 40000 + Sock->ID; // Port dinamis sederhana
        Sock->RemotePort = TargetPort;
        
        // [SECURE] Use RNG for ISN (Initial Sequence Number)
        Sock->MySequence = EntrophySystem::Random();
        Sock->TargetSequence = 0;

        if (Domain && Sock->Domain) {
            String::Strcpy(Sock->Domain, Domain);
        } else if (Sock->Domain) {
            Sock->Domain[0] = 0;
        }

        Printk::Write(Printk::Level::LOG_DEBUG, "[TCP] Socket %d connectiong to %s...\n", Sock->ID, Domain ? Domain : "IP");

        SendTCPRaw(Card, Sock, TCP_SYN, nullptr, 0);
        Sock->State = SYN_SENT;

        return Sock;
    }

    VOID HandleTCP(NetworkInterface* Card, U8* Data, U32 Len, U32 SrcIP) {
        TCPHeader* Tcp = (TCPHeader*)Data;

        U16 DestPort = ntohs(Tcp->DestPort);
        U16 SrcPort = ntohs(Tcp->SrcPort);
        U32 Seq = ntohl(Tcp->SequenceNum);
        U32 Ack = ntohl(Tcp->AckNum);
        U8 Flags = Tcp->Flags;

        TCPSocket* Sock = GetSocket(DestPort, SrcIP, SrcPort);

        if(!Sock){
            TCPSocket *Listener = GetListener(DestPort);

            if(Listener && (Flags & TCP_SYN)){
                Printk::Write(Printk::Level::LOG_DEBUG, "[TCP] New connection request on port %d.\n", DestPort);

                Sock = AllocateSocket();
                if(Sock){
                    // Copy info dasar
                    Sock->LocalIP = Card->IP;
                    Sock->LocalPort = DestPort;
                    
                    // Set info lawan
                    Sock->RemoteIP = SrcIP;
                    Sock->RemotePort = SrcPort;
                    
                    // Set Sequence
                    Sock->TargetSequence = Seq + 1;
                    
                    // [SECURE] Use RNG for ISN
                    Sock->MySequence = EntrophySystem::Random();
                    
                    // Kirim SYN-ACK dari socket BARU ini
                    SendTCPRaw(Card, Sock, TCP_SYN | TCP_ACK, nullptr, 0);
                    Sock->State = SYN_RCVD;
                    Sock->MySequence++;
                    
                    // Listener tetep LISTEN, Child yang ngurus koneksi ini.
                    return;
                }
            }
        }

        if(!Sock){
            // Printk::Write(Printk::Level::LOG_WARNING, ...); // Matikan log ini kalau berisik

            // --- LOGIC ANTI-BAPER (Kirim RST) ---
            // Kalau bukan SYN, berarti ini paket nyasar/zombie. Kita RESET.
            if (!(Flags & TCP_RST)) { // Jangan bales RST dengan RST (nanti ping-pong infinity)
                
                // Tukar Port & IP buat balesan
                TCPSocket TempSock;
                TempSock.LocalPort = DestPort;
                TempSock.RemotePort = SrcPort;
                TempSock.LocalIP = Card->IP;
                TempSock.RemoteIP = SrcIP;
                
                // Logic Sequence RST:
                // Kalau dia kirim ACK, kita pake Seq = Ack dia.
                TempSock.MySequence = Ack;
                TempSock.TargetSequence = Seq + 1; // Basa-basi

                // Kirim RST (Reset) | ACK
                SendTCPRaw(Card, &TempSock, TCP_RST | TCP_ACK, nullptr, 0);
            }
            return;
        }  

        switch (Sock->State) {
            case LISTEN:{
                if(Flags & TCP_SYN){
                    Printk::Write(Printk::Level::LOG_INFO, "[SERVER] Got SYN from IP %x\n", SrcIP);
                    
                    Sock->RemoteIP = SrcIP;
                    Sock->RemotePort = ntohs(Tcp->SrcPort);
                    Sock->TargetSequence = Seq + 1; 
                    
                    // [SECURE] Use RNG for ISN
                    Sock->MySequence = EntrophySystem::Random();
                    
                    // Kirim SYN + ACK
                    SendTCPRaw(Card, Sock, TCP_SYN | TCP_ACK, nullptr, 0);
                    
                    Sock->State = SYN_RCVD;
                    Sock->MySequence++; // Increment SETELAH kirim SYN
                }
                break;
            }

            case SYN_SENT:{
                // Kita nunggu balasan SYN + ACK dari Google
                Printk::Write(Printk::Level::LOG_DEBUG, "[CLIENT] In SYN_SENT, Flags=0x%02x\n", Flags);
                if ((Flags & TCP_SYN) && (Flags & TCP_ACK)) {
                    Printk::Write(Printk::Level::LOG_INFO, "[CLIENT] Handshake Success! Connected to %x\n", SrcIP);
                    
                    // 1. Update Sequence
                    Sock->RemoteIP = SrcIP; // Pastikan IP Lawan kekunci
                    Sock->TargetSequence = Seq + 1;
                    Sock->MySequence++; // SYN kita makan 1 seq
                    
                    // 2. Kirim ACK (Sah-kan hubungan)
                    SendTCPRaw(Card, Sock, TCP_ACK, nullptr, 0);
                    
                    // 3. Ubah Status jadi ESTABLISHED
                    Sock->State = ESTABLISHED;

                    // 4. LANGSUNG KIRIM HTTP GET DISINI!
                    Printk::Write(Printk::Level::LOG_INFO, "[CLIENT] Sending HTTP GET...\n");
                    
                    char HttpRequest[512];
                    const char* Host = (Sock->Domain && Sock->Domain[0] != 0) ? Sock->Domain : "unknown";
                    
                    // Construct HTTP Request
                    String::Strcpy(HttpRequest, "GET / HTTP/1.1\r\nHost: ");
                    String::Strcat(HttpRequest, Host);
                    String::Strcat(HttpRequest, "\r\nConnection: close\r\n\r\n");

                    Sock->MySequence++; // ACK kita gak makan seq, tapi biar konsisten
                    SendTCPData(Card, Sock, (U8*)HttpRequest, String::Strlen(HttpRequest));
                }
                // Kalau cuma terima SYN (Simultaneous Open - jarang terjadi)
                else if (Flags & TCP_SYN) {
                    Printk::Write(Printk::Level::LOG_DEBUG, "[CLIENT] Simultaneous Open detected. Sending SYN-ACK...\n");
                    Sock->TargetSequence = Seq + 1;
                    Sock->MySequence++;
                    SendTCPRaw(Card, Sock, TCP_ACK, nullptr, 0);
                    Sock->State = SYN_RCVD;
                }
                break;
            }

            case SYN_RCVD:{
                // [FIX 1] Handle Retransmission SYN
                // Kalau terima SYN lagi, berarti ACK kita hilang/telat. Kirim ulang.
                Printk::Write(Printk::Level::LOG_DEBUG, "[TCP] In SYN_RCVD, Flags=0x%02x\n", Flags);
                if (Flags & TCP_SYN) {
                    Printk::Write(Printk::Level::LOG_WARNING, "[SERVER] Retransmitting SYN-ACK...\n");
                    
                    // PENTING: SYN memakan 1 Sequence. Saat retransmisi, kita harus pakai Sequence ASLI (sebelum di ++).
                    Sock->MySequence--; 
                    SendTCPRaw(Card, Sock, TCP_SYN | TCP_ACK, nullptr, 0);
                    Sock->MySequence++; // Balikin lagi
                    return; // Jangan lanjut proses
                }

                // Cek ACK dari Client
                if ((Flags & TCP_ACK) && (Ack == Sock->MySequence)) {
                    
                    Sock->State = ESTABLISHED;
                    
                    // [FIX] Stop Retransmission Timer karena Handshake selesai
                    Sock->WaitingAck = FALSE;
                    if (Sock->LastPacket) {
                        Kmalloc::Free(Sock->LastPacket);
                        Sock->LastPacket = nullptr;
                    }

                    Printk::Write(Printk::Level::LOG_INFO, "[SERVER] Handshake complete! Connection ESTABLISHED.\n");
                    
                    // [FIX] JANGAN BREAK! Lanjut ke case ESTABLISHED di bawah (Fallthrough)
                    // Siapa tau paket ACK ini juga bawa Data (seperti HTTP GET yang dipiggyback)
                } else {
                    break; // Kalau bukan ACK yang valid, keluar.
                }
            }

            case ESTABLISHED:
                {
                    U8 HeaderLen = (Tcp->DataOffset >> 4) * 4;
                    U32 DataLen = Len - HeaderLen;
                    U8* Payload = Data + HeaderLen;

                    if(DataLen > 0){
                        if (Seq == Sock->TargetSequence) {
                            // --- RECEIVE BUFFER LOGIC ---
                            // Masukkan data ke Ring Buffer
                            for(U32 i=0; i<DataLen; i++) {
                                Sock->RecvBuffer[Sock->RecvWritePtr] = Payload[i];
                                Sock->RecvWritePtr = (Sock->RecvWritePtr + 1) % Sock->RecvBufferSize;
                                
                                // Kalau buffer penuh, overwrite data lama (Simple Ring Buffer)
                                // Idealnya: Drop paket dan kirim Zero Window.
                                if (Sock->RecvWritePtr == Sock->RecvReadPtr) {
                                    Sock->RecvReadPtr = (Sock->RecvReadPtr + 1) % Sock->RecvBufferSize;
                                }
                            }

                            Sock->TargetSequence += DataLen;

                            ProcessOOOList(Sock); // Cek apakah ada segmen OOO yang bisa diproses

                            SendTCPRaw(Card, Sock, TCP_ACK, nullptr, 0);
                        } else if (Seq > Sock->TargetSequence) {
                            // Simpan dulu!
                            AddOOOSegment(Sock, Seq, Payload, DataLen);
                            
                            // Kita kirim ACK duplicate (ACK angka terakhir yg valid)
                            // Biar pengirim tau kita minta paket yg ilang.
                            SendTCPRaw(Card, Sock, TCP_ACK, nullptr, 0);

                            return; 
                        } else {
                            // Paket lama (duplikat), abaikan
                            SendTCPRaw(Card, Sock, TCP_ACK, nullptr, 0);
                            Printk::Write(Printk::Level::LOG_DEBUG, "[TCP] Received old/duplicate segment. Ignoring.\n");
                        }

                        if (DataLen >= 5 && String::Strncmp((char*)Payload, "GET /", 5) == 0) {
                            Printk::Write(Printk::Level::LOG_INFO, "[HTTP] HTTP Request Detected! Sending Response...\n");

                            const char* HtmlResponse = 
                                "HTTP/1.1 200 OK\r\n"
                                "Content-Type: text/html\r\n"
                                "Connection: close\r\n" // Bilang ke browser kita mau putus
                                "\r\n"
                                "<html><head><title>R-OS</title></head>"
                                "<body><h1 style='color:blue'>Hello from R-OS Kernel!</h1>"
                                "<p>TCP Stack is working!</p></body></html>";

                            // Kirim Balasan
                            SendTCPData(Card, Sock, (U8*)HtmlResponse, String::Strlen(HtmlResponse));
                            
                            // Mulai proses penutupan koneksi (Active Close)
                            // Kirim FIN setelah data terkirim
                            SendTCPRaw(Card, Sock, TCP_FIN | TCP_ACK, nullptr, 0);
                            Sock->MySequence++; // FIN consumes 1 sequence number
                            Sock->State = FIN_WAIT_1;
                        }
                    }

                    // Kalau ACK yang kita tunggu sudah datang
                    if (Sock->WaitingAck && Ack == Sock->MySequence) {
                        Sock->WaitingAck = FALSE; // Stop Retransmission Timer
                        if (Sock->LastPacket) {
                            Kmalloc::Free(Sock->LastPacket);
                            Sock->LastPacket = nullptr;
                        }
                    }

                    if (Flags & TCP_FIN) {
                        Printk::Write(Printk::Level::LOG_INFO, "[TCP] Received FIN from Remote. Closing...\n");
                        
                        // 1. ACK FIN lawan (Seq lawan nambah 1 karena FIN makan 1 seq)
                        Sock->TargetSequence++;
                        SendTCPRaw(Card, Sock, TCP_ACK, nullptr, 0);
                        
                        // 2. Kirim FIN kita sendiri (Active Close / Passive Close)
                        SendTCPRaw(Card, Sock, TCP_FIN | TCP_ACK, nullptr, 0);
                        
                        // 3. Pindah State
                        Sock->State = LAST_ACK; // Tunggu ACK terakhir dari lawan
                        return;
                    }
                    break;
                }
                
                
            // State Sederhana buat nunggu FIN terakhir
            case FIN_WAIT_1:{
                if (Flags & TCP_ACK) {
                    // Cek apakah ACK ini meng-acknowledge FIN kita
                    if (Sock->WaitingAck && Ack == Sock->MySequence) {
                        Sock->WaitingAck = FALSE;
                        if (Sock->LastPacket) {
                            Kmalloc::Free(Sock->LastPacket);
                            Sock->LastPacket = nullptr;
                        }
                    }

                    Sock->State = FIN_WAIT_2;
                    Printk::Write(Printk::Level::LOG_DEBUG, "[TCP] Moved to FIN_WAIT_2.\n");
                    // Fallthrough untuk cek FIN (kalau FIN+ACK)
                }
                if (Flags & TCP_FIN) {
                    Sock->TargetSequence++;
                    SendTCPRaw(Card, Sock, TCP_ACK, nullptr, 0);
                    FreeSocket(Sock);
                    Printk::Write(Printk::Level::LOG_INFO, "[TCP] Clean Close. Socket Freed.\n");
                    return;
                }
                break;
            }

            case FIN_WAIT_2:{
                if (Flags & TCP_FIN) {
                    Printk::Write(Printk::Level::LOG_INFO, "[TCP] Received final FIN from client. Closing connection.\n");
                    
                    // ACK FIN client
                    Sock->TargetSequence++;
                    SendTCPRaw(Card, Sock, TCP_ACK, nullptr, 0);
                    
                    // Kita harusnya pindah ke TIME_WAIT, tapi untuk OS sederhana, kita langsung Free.
                    FreeSocket(Sock);
                    Printk::Write(Printk::Level::LOG_DEBUG, "[TCP] Connection Closed. Socket Freed.\n");
                    return;
                }
                break;
            }

            case LAST_ACK: {
                // Kita menunggu ACK terakhir untuk FIN kita
                if (Flags & TCP_ACK) {
                     Printk::Write(Printk::Level::LOG_INFO, "[TCP] Received final ACK in LAST_ACK. Connection Closed.\n");
                     FreeSocket(Sock);
                     return;
                }
                break;
            }

            default:
                Printk::Write(Printk::Level::LOG_DEBUG, "[TCP] Unknown state %d\n", Sock->State);
                break;
        }
    }

    // API: Receive Data dari Socket (Blocking / Non-Blocking tergantung implementasi)
    // Return: Jumlah bytes yang dibaca
    INTN TCPRecv(TCPSocket* Sock, U8* Buffer, U32 Len) {
        if (!Sock || !Sock->Allocated) return -1;

        BOOL WasZeroWindow = FALSE;
        U32 UsedBuffer = (Sock->RecvWritePtr >= Sock->RecvReadPtr) ? 
                         (Sock->RecvWritePtr - Sock->RecvReadPtr) : 
                         (Sock->RecvBufferSize - (Sock->RecvReadPtr - Sock->RecvWritePtr));
        if ((Sock->RecvBufferSize - UsedBuffer) < 1460) WasZeroWindow = TRUE;
        
        U32 BytesRead = 0;
        
        // Loop ambil data dari Ring Buffer
        while (BytesRead < Len) {
            if (Sock->RecvReadPtr == Sock->RecvWritePtr) {
                break; // Buffer kosong
            }
            
            Buffer[BytesRead++] = Sock->RecvBuffer[Sock->RecvReadPtr];
            Sock->RecvReadPtr = (Sock->RecvReadPtr + 1) % Sock->RecvBufferSize;
        }

        if (WasZeroWindow && BytesRead > 0) {
            // Kirim Window Update (ACK dengan Seq/Ack sama, tapi WindowSize baru)
            // SendTCPRaw otomatis hitung WindowSize baru dari buffer saat ini.
            SendTCPRaw(Network::E1000::g_E1000Instance, Sock, TCP_ACK, nullptr, 0);
            Printk::Write(Printk::Level::LOG_DEBUG, "[TCP] Window Update sent.\n");
        }
        
        return BytesRead;
    }
}