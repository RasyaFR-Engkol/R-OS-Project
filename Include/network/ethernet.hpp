#pragma once

#include <rosval.h>
#include <network/netinterface.hpp>

namespace Network{

    #define MAX_ICMP_SOCKETS 16

    struct EthernetHeader {
        U8 Dest[6];
        U8 Src[6];
        U16 Type; // EtherType
    } PACKSTRUCT; // Pastikan ini packed!

    struct PendingPacket {
        BOOL Valid;
        U8 Protocol;   // 1=ICMP, 17=UDP
        U32 TargetIP;
        U16 SrcPort;
        U16 DestPort;
        U8* Payload;    // Kita simpan copy datanya
        U32 PayloadLen;
    };

    struct ICMPSocket {
        BOOL Allocated;
        U16 ID; // Identifier (pengganti Port di ICMP)

        U32 RemoteIP;   // Diset via connect()
        U16 Sequence;   // Auto-increment tiap kirim
        
        U8* RecvBuffer;
        U32 RecvBufferSize;
        U32 RecvWritePtr;
        U32 RecvReadPtr;
    };

    extern PendingPacket g_PendingUDP;

    U32 ICMPRecv(ICMPSocket* Sock, U8* Buffer, U32 MaxLen) ;
    ICMPSocket* ICMPOpen(U16 ID);
    VOID ICMPClose(ICMPSocket* Sock);
    VOID TestNetwork();
    VOID BroadcastARP(U32 TargetIP);
    BOOL GetMacFromCache(U32 TargetIP, U8 *OutMac);
    VOID SendARPRequest(NetworkInterface* Card, U32 TargetIP);;
}