#pragma once
#include <rosval.h>
#include <network/netinterface.hpp>

namespace Network {
    struct UDPSocket {
        BOOL Allocated;
        U16 LocalPort;
        U16 LocalIP;  // Opsional
        U32 RemoteIP;   // Opsional (untuk connect())
        U16 RemotePort; // Opsional

        U16 Id;
        
        // Buffer untuk data masuk
        U8* RecvBuffer;
        U32 RecvBufferSize;
        U32 RecvWritePtr;
        U32 RecvReadPtr;
        
        // Metadata paket terakhir (siapa pengirimnya?)
        // Penting buat recvfrom() biar user tau ini data dari mana
        struct {
            U32 SrcIP;
            U16 SrcPort;
            U32 Length;
        } LastPacketInfo;
    };

    #define MAX_UDP_SOCKETS 32
    extern UDPSocket UdpSocketTable[MAX_UDP_SOCKETS];

    UDPSocket* AllocateUDPSocket();
    void FreeUDPSocket(UDPSocket* sock);
    UDPSocket* GetUDPSocket(U16 LocalPort);
    UDPSocket* UDPListen(U16 Port);
    U32 UDPRecv(UDPSocket* Sock, U8* Buffer, U32 MaxLen);
    VOID UDPClose(UDPSocket* Sock);
    
    // Fungsi untuk mengirim data dari socket
    void SendUDPSocket(UDPSocket* sock, U32 DestIP, U16 DestPort, U8* Data, U32 Len);
    void UDPReceive(UDPSocket* sock, U32 SrcIP, U16 SrcPort, U8* Data, U32 Len);
}