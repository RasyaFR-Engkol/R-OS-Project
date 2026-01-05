#pragma once
#include "network/netinterface.hpp"
#include <rosval.h>
#include <network/ipv4.hpp>
#include <network/ethernet.hpp>

namespace Network{

    #define DHCP_OP_REQUEST 1
    #define DHCP_OP_REPLY   2

    #define DHCP_MAGIC_COOKIE 0x63825363

    struct DHCPHeader {
        U8  Op;         // 1 = Boot Request, 2 = Boot Reply
        U8  HType;      // Hardware type (1 = Ethernet)
        U8  HLen;       // Hardware len (6)
        U8  Hops;       // 0
        U32 Xid;        // Transaction ID (Random number unik)
        U16 Secs;       // Seconds elapsed
        U16 Flags;      // Broadcast flag
        U32 CIAddr;     // Client IP (kalau udah punya)
        U32 YIAddr;     // Your IP (IP yang ditawarkan server)
        U32 SIAddr;     // Server IP
        U32 GIAddr;     // Gateway IP
        U8  CHAddr[16]; // Client Hardware Address (MAC kita + padding)
        U8  SName[64];  // Server Hostname
        U8  File[128];  // Boot filename
        U32 MagicCookie;// Wajib: 0x63825363
    } PACKSTRUCT;

    struct DHCPClientInfo {
        U32 RequestedIP;
        U32 ServerIP;
        U32 SubnetMask;
        U32 GatewayIP;
        U32 DnsServer;
    };

    extern DHCPClientInfo g_DHCPInfo;

    VOID InitDHCP();
    VOID SendDHCPDiscover(NetworkInterface* Card);
    VOID HandleDHCP(NetworkInterface* Card, U8* Data, U32 Length);
}