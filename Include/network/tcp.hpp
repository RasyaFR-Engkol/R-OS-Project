#pragma once
#include <rosval.h>
#include <network/netinterface.hpp>

namespace Network{
    #define TCP_FIN 0x01
    #define TCP_SYN 0x02
    #define TCP_RST 0x04
    #define TCP_PSH 0x08
    #define TCP_ACK 0x10
    #define TCP_URG 0x20

    struct TCPHeader{
        U16 SrcPort;
        U16 DestPort;
        U32 SequenceNum;    // Nomor urut paket saya
        U32 AckNum;         // Nomor urut paket yang saya terima dari lawan
        
        U8  DataOffset;     // Panjang Header (dikali 4 bytes). Biasanya 5 (20 bytes).
                            // Tapi ini tricky, karena nyampur sama Reserved bits/NS flag.
                            // Nanti kita set manual pake bitwise.
        
        U8  Flags;          // SYN, ACK, FIN, dll
        U16 WindowSize;     // "Gw sanggup nerima berapa byte lagi"
        U16 Checksum;
        U16 UrgentPtr;
    } PACKSTRUCT;

    enum TCPState{
        CLOSED,
        LISTEN,
        SYN_SENT,
        SYN_RCVD,
        ESTABLISHED,
        FIN_WAIT_1,
        FIN_WAIT_2,
        LAST_ACK,
        TIME_WAIT,
        CLOSE_WAIT
    };

    struct TCPSegmentNode {
        U32 Sequence;
        U32 Length;
        U8* Payload; // Copy data
        TCPSegmentNode* Next;
    };

    struct TCPSocket {
        U16 ID;
        BOOL Allocated;

        TCPState State;
        U32 LocalIP;
        U32 RemoteIP;
        U16 LocalPort;
        U16 RemotePort;
        
        U32 MySequence;     // SEQ kita saat ini
        U32 TargetSequence; // SEQ lawan yang kita harapkan (ACK)
        
        // Retransmission Timer
        U32 LastSendTime;   // Waktu terakhir kirim paket (Ticks)
        BOOL WaitingAck;    // Apakah kita sedang menunggu ACK?
        U8*  LastPacket;    // Pointer ke copy paket terakhir (untuk retransmisi)
        U16  LastPacketLen; // Panjang paket terakhir
        U8   LastFlags;     // Flags paket terakhir

        // Receive Buffer (Ring Buffer Sederhana)
        U8*  RecvBuffer;
        U32  RecvWritePtr;
        U32  RecvReadPtr;
        U32  RecvBufferSize;

        char* Domain;       // Domain name (Allocated dynamically)
        BOOL IsListening;

        TCPSegmentNode *OOOList;
    };

    #define MAX_TCP_SOCKETS 128

    VOID HandleTCP(NetworkInterface* Card, U8* Data, U32 Len, U32 SrcIP);
    VOID SendTCPData(NetworkInterface* Card, TCPSocket *Sock, U8* Data, U16 Len);
    VOID TCPListen(NetworkInterface* Card, U16 Port);
    TCPSocket *TCPConnect(NetworkInterface* Card, U32 TargetIP, U16 TargetPort, const char* Domain = nullptr);
    INTN TCPRecv(TCPSocket* Sock, U8* Buffer, U32 Len);
    VOID TCPCheckRetransmission(NetworkInterface* Card); // Panggil ini di timer interrupt / loop utama
    VOID FreeSocket(TCPSocket *Sock);
    TCPSocket *AllocateSocket();
    VOID SendTCPRaw(NetworkInterface* Card, TCPSocket *Sock, U8 Flags, U8 *Payload, U16 PayloadLen);
} 