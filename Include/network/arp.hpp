#pragma once

#include <rosval.h>

namespace Network{
    #define ARP_OP_REQUEST 1
    #define ARP_OP_REPLY   2

    #define ETHER_TYPE_IP 0x0800
    #define ETHER_TYPE_ARP 0x0806

    struct ARPHeader{
        U16 HardwareType;      // Hardware Type (1 = Ethernet)
        U16 ProtocolType;      // Protocol Type (0x0800 = IPv4)
        U8  HardwareAddrLen;      // Hardware Address Length (6 for MAC)
        U8  ProtocolAddrLen;      // Protocol Address Length (4 for IPv4)
        U16 OpCode;         // Operation (1 = request, 2 = reply)
        U8  SrcMac[6];     // Sender hardware address
        U32  SrcIp;
        U8  DestMac[6];     // Target hardware address
        U32  DestIP;
    } PACKSTRUCT ;
}