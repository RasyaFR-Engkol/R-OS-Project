#pragma once
#include <rosval.h>

namespace Network{
    class NetworkInterface;

    typedef VOID (*PacketHandler)(NetworkInterface *Card, U8 *Data, U32 Length);
    
    class NetworkInterface{
        public:
            U8 Mac[6];
            U32 IP;
            U32 Gateway;
            U32 SubnetMask;
            U32 DnsServer;

            virtual BOOL SendRawPacket(U8 *Data, U32 Length) = 0;

            VOID SetPacketHandler(PacketHandler Handler){
                this->ReceiveHandler = Handler;
            }

            VOID OnReceive(U8 *Data, U32 Length){
                if(ReceiveHandler){
                    ReceiveHandler(this, Data, Length);
                }
            }

        protected:
            PacketHandler ReceiveHandler;
    };

    VOID EthernetInput(NetworkInterface *Card, U8 *Data, U32 Length);
    BOOL SendUDP(NetworkInterface* Card, U32 DestIP, U16 SrcPort, U16 DestPort, U8* Data, U16 Len);
    BOOL SendICMP(NetworkInterface* Card, U32 DestIP, U16 Seq, U16 ID, U8* Data, U16 Len);
    VOID SendPing(NetworkInterface *Card, U32 TargetIP, U8 *TargetMac);
}