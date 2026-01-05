#include <network/dhcp.hpp>
#include <network/netinterface.hpp>
#include <network/ethernet.hpp>
#include <network/ipv4.hpp>
#include <network/swapper.hpp>
#include <rosval.h>
#define PRINTK_MODULE_NAME "DHCP"
#include <logging.hpp>
#include <mm.hpp>
#include <string.hpp>

namespace Network{
    using namespace Printk;

    // JANGAN DI-INCREMENT DI DALAM FUNGSI! Biar konsisten satu transaksi.
    STATIC U32 CurrentXID = 0x12345678; 

    DHCPClientInfo g_DHCPInfo; 

    enum DHCPState {
        DHCP_NONE,
        DHCP_DISCOVER_SENT,
        DHCP_REQUEST_SENT,
        DHCP_BOUND
    };

    static DHCPState g_State = DHCP_NONE;

    U8* AddOption(U8* Ptr, U8 Type, U8 Length, U8* Value) {
        *Ptr++ = Type;
        *Ptr++ = Length;
        String::Memcpy(Ptr, Value, Length);
        return Ptr + Length;
    }

    VOID SendDHCPDiscover(NetworkInterface *Card){
        U32 PktSize = sizeof(DHCPHeader) + 312; 
        U8 *Buffer = (U8*)Kmalloc::Alloc(PktSize);
        String::Memset(Buffer, 0, PktSize);

        DHCPHeader *DhcpHdr = (DHCPHeader*)Buffer;
        DhcpHdr->Op = DHCP_OP_REQUEST;
        DhcpHdr->HType = 1; 
        DhcpHdr->HLen = 6;  
        DhcpHdr->Xid = htonl(CurrentXID); // Pake XID Global
        String::Memcpy(DhcpHdr->CHAddr, Card->Mac, 6);
        DhcpHdr->MagicCookie = htonl(DHCP_MAGIC_COOKIE);

        U8* OptPtr = (U8*)(DhcpHdr + 1);

        U8 MsgType = 1; // DISCOVER
        OptPtr = AddOption(OptPtr, 53, 1, &MsgType);

        // Minta: Subnet(1), Router(3), DNS(6)
        U8 ReqList[] = {1, 3, 6}; 
        OptPtr = AddOption(OptPtr, 55, 3, ReqList);

        *OptPtr++ = 255; // End Option

        U32 TotalLen = (U32)(OptPtr - Buffer);
        Write(Level::LOG_INFO, " DHCP: Sending DHCP Discover...\n");
        g_State = DHCP_DISCOVER_SENT;

        SendUDP(Card, 0xFFFFFFFF, 68, 67, Buffer, TotalLen);
        Kmalloc::Free(Buffer);
    }

    VOID SendDHCPRequest(NetworkInterface* Card, U32 RequestedIP, U32 ServerIP) {
        U32 PktSize = sizeof(DHCPHeader) + 312;
        U8* Buffer = (U8*)Kmalloc::Alloc(PktSize);
        String::Memset(Buffer, 0, PktSize);

        DHCPHeader* Dhcp = (DHCPHeader*)Buffer;
        Dhcp->Op = DHCP_OP_REQUEST;
        Dhcp->HType = 1;
        Dhcp->HLen = 6;
        Dhcp->Xid = htonl(CurrentXID); // XID harus sama dengan Discover
        String::Memcpy(Dhcp->CHAddr, Card->Mac, 6);
        Dhcp->MagicCookie = htonl(DHCP_MAGIC_COOKIE);

        U8* OptPtr = (U8*)(Dhcp + 1);

        // Option 53: REQUEST
        U8 MsgType = 3;
        OptPtr = AddOption(OptPtr, 53, 1, &MsgType);

        // Option 50: Requested IP
        OptPtr = AddOption(OptPtr, 50, 4, (U8*)&RequestedIP);

        // Option 54: Server Identifier
        OptPtr = AddOption(OptPtr, 54, 4, (U8*)&ServerIP);

        // --- INI YANG KEMAREN KURANG! ---
        // Kita WAJIB minta Router lagi saat Request, 
        // kalau gak diminta, Server gak bakal masukin di ACK.
        U8 ReqList[] = {1, 3, 6}; 
        OptPtr = AddOption(OptPtr, 55, 3, ReqList);
        // --------------------------------

        *OptPtr++ = 255; // END

        Write(Level::LOG_INFO, "[DHCP] Sending REQUEST for %x...\n", RequestedIP);
        g_State = DHCP_REQUEST_SENT;

        SendUDP(Card, 0xFFFFFFFF, 68, 67, Buffer, (U32)(OptPtr - Buffer));
        Kmalloc::Free(Buffer);
    }

    VOID HandleDHCP(NetworkInterface* Card, U8* Data, U32 Length){
        if(Length < sizeof(DHCPHeader)) return;

        DHCPHeader* Dhcp = (DHCPHeader*)Data;

        // Cek XID dan Reply Op
        if(ntohl(Dhcp->Xid) != CurrentXID) return;
        if(Dhcp->Op != DHCP_OP_REPLY) return;

        U8* OptPtr = (U8*)(Dhcp + 1);
        U8 MsgType = 0;
        U32 ServerIP = 0;
        U32 SubnetMask = 0;
        U32 Gateway = 0;
        U32 DnsServer = 0; // Tambah variabel lokal buat DNS

        // Parsing Option
        while((OptPtr < Data + Length) && (*OptPtr != 255)){
            U8 OptCode = *OptPtr++;
            if (OptCode == 0) continue; 
            U8 OptLen = *OptPtr++;

            switch(OptCode){
                case 53: MsgType = *OptPtr; break;
                case 1:  String::Memcpy(&SubnetMask, OptPtr, 4); break;
                case 3:  String::Memcpy(&Gateway, OptPtr, 4); break; // Router
                case 6:  String::Memcpy(&Card->DnsServer, OptPtr, 4); break; // DNS
                case 54: String::Memcpy(&ServerIP, OptPtr, 4); break; // Server ID
            }
            OptPtr += OptLen;
        }

        if (g_State == DHCP_DISCOVER_SENT && MsgType == 2) { // OFFER
            Write(Level::LOG_INFO, "[DHCP] Got OFFER: IP %x. Requesting...\n", Dhcp->YIAddr);

            g_DHCPInfo.ServerIP = ServerIP;

            SendDHCPRequest(Card, Dhcp->YIAddr, ServerIP);
        }
        else if (g_State == DHCP_REQUEST_SENT && MsgType == 5) { // ACK
            Write(Level::LOG_INFO, "[DHCP] Got ACK! Config applied.\n");
            
            Card->IP = Dhcp->YIAddr;
            Card->SubnetMask = SubnetMask;
            Card->Gateway = Gateway; // <-- Gateway dari Option 3 (ACK) dipasang disini
            Card->DnsServer = DnsServer; // Pasang DNS juga 

            g_DHCPInfo.RequestedIP = Dhcp->YIAddr; // IP Kita
            g_DHCPInfo.SubnetMask  = SubnetMask;
            g_DHCPInfo.GatewayIP   = Gateway;
            g_DHCPInfo.DnsServer   = DnsServer;
            g_DHCPInfo.ServerIP    = ServerIP; 
            
            g_State = DHCP_BOUND;
            
            U8* GwPtr = (U8*)&Card->Gateway;
            Write(Level::LOG_INFO, "[NET] IP: %d.%d.%d.%d\n", 
                (Card->IP >> 0) & 0xFF, (Card->IP >> 8) & 0xFF, 
                (Card->IP >> 16) & 0xFF, (Card->IP >> 24) & 0xFF);
            Write(Level::LOG_INFO, "[NET] Gateway: %d.%d.%d.%d\n", 
                GwPtr[0], GwPtr[1], GwPtr[2], GwPtr[3]);
        }   
    }
}