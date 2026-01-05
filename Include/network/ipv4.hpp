#pragma once
#include <rosval.h>

namespace Network {

    #define IP_PROTOCOL_ICMP 1
    #define IP_PROTOCOL_TCP  6
    #define IP_PROTOCOL_UDP  17

    struct IPv4Header {
        U8  VersionIHL;     // Version (4 bits) + Internet Header Length (4 bits)
        U8  TOS;            // Type of Service
        U16 TotalLength;    // Length of header + data
        U16 ID;             // Identification
        U16 FlagsFragment;  // Flags (3 bits) + Fragment Offset (13 bits)
        U8  TTL;            // Time to Live
        U8  Protocol;       // ICMP, TCP, UDP
        U16 HeaderChecksum; 
        U32 SrcIP;
        U32 DestIP;
    } __attribute__((packed));

    struct ICMPHeader {
        U8  Type; // 8 = Echo Request (Ping), 0 = Echo Reply
        U8  Code; // 0
        U16 Checksum;
        U16 ID;
        U16 Sequence;
        // Data follows...
    } __attribute__((packed));

    struct UDPHeader {
        U16 SrcPort;
        U16 DestPort;
        U16 Length;    // Length of UDP Header + Data
        U16 Checksum;  // Optional di IPv4, tapi kita hitung aja biar proper
    } __attribute__((packed));

    // Pseudo Header untuk hitung Checksum UDP (Wajib di standar Internet)
    struct PseudoHeader {
        U32 SrcIP;
        U32 DestIP;
        U8  Reserved;
        U8  Protocol;
        U16 Length;
    } __attribute__((packed));

    namespace IPv4{
        static inline U32 ToU32(U8 ip1, U8 ip2, U8 ip3, U8 ip4) {
            // Geser bit-nya biar berjejer
            // IP: 192.168.1.1
            // ip1 (192) ditaruh di paling kiri (High Byte)
            return (U32)ip1 << 24 | (U32)ip2 << 16 | (U32)ip3 << 8 | (U32)ip4;
        }
    }
}