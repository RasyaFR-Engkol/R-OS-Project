#pragma once

// --- Address Families (Domain) ---
#define AF_UNSPEC   0
#define AF_UNIX     1   // Unix Domain Socket (Inter-process)
#define AF_INET     2   // IPv4 Internet Protocols
#define AF_INET6    10  // IPv6 Internet Protocols

// --- Socket Types (Type) ---
#define SOCK_STREAM 1   // TCP (Reliable, Connection-oriented)
#define SOCK_DGRAM  2   // UDP (Unreliable, Connectionless)
#define SOCK_RAW    3   // Raw Protocol Interface (ICMP, Custom IP)

// --- IP Protocols (Protocol) ---
#define IPPROTO_IP      0   // Dummy protocol for TCP
#define IPPROTO_ICMP    1   // Internet Control Message Protocol
#define IPPROTO_TCP     6   // Transmission Control Protocol
#define IPPROTO_UDP     17  // User Datagram Protocol
#define IPPROTO_RAW     255 // Raw IP Packet

// --- Special IP Addresses ---
#define INADDR_ANY          0x00000000 // 0.0.0.0 (Listen on all interfaces)
#define INADDR_BROADCAST    0xFFFFFFFF // 255.255.255.255
#define INADDR_LOOPBACK     0x7F000001 // 127.0.0.1
#define INADDR_NONE         0xFFFFFFFF

// --- Ethernet Types ---
#define ETHERTYPE_IP    0x0800  // IPv4
#define ETHERTYPE_ARP   0x0806  // Address Resolution Protocol
#define ETHERTYPE_IPV6  0x86DD  // IPv6

// --- ARP Operations ---
#define ARP_OP_REQUEST  1
#define ARP_OP_REPLY    2

// --- ICMP Types ---
#define ICMP_ECHO_REPLY     0
#define ICMP_DEST_UNREACH   3
#define ICMP_SOURCE_QUENCH  4
#define ICMP_REDIRECT       5
#define ICMP_ECHO_REQUEST   8
#define ICMP_TIME_EXCEEDED  11

// --- ICMP Codes (untuk Destination Unreachable) ---
#define ICMP_CODE_NET_UNREACH   0
#define ICMP_CODE_HOST_UNREACH  1
#define ICMP_CODE_PROTO_UNREACH 2
#define ICMP_CODE_PORT_UNREACH  3 // Sering muncul kalau UDP port tutup

// --- TCP Flags ---
#define TCP_FIN     0x01
#define TCP_SYN     0x02
#define TCP_RST     0x04
#define TCP_PSH     0x08
#define TCP_ACK     0x10
#define TCP_URG     0x20
#define TCP_ECE     0x40
#define TCP_CWR     0x80

// --- TCP Options ---
#define TCP_OPT_END     0
#define TCP_OPT_NOP     1
#define TCP_OPT_MSS     2   // Max Segment Size
#define TCP_OPT_WS      3   // Window Scale
#define TCP_OPT_SACK_P  4   // SACK Permitted

// --- Common Ports ---
#define PORT_FTP_DATA   20
#define PORT_FTP_CTRL   21
#define PORT_SSH        22
#define PORT_TELNET     23
#define PORT_SMTP       25
#define PORT_DNS        53
#define PORT_HTTP       80
#define PORT_HTTPS      443

// --- DHCP Ports ---
#define PORT_DHCP_SERVER    67
#define PORT_DHCP_CLIENT    68

// --- DHCP Operations ---
#define DHCP_OP_REQUEST     1
#define DHCP_OP_REPLY       2

// --- DHCP Magic Cookie ---
#define DHCP_MAGIC_COOKIE   0x63825363

// --- DHCP Message Types (Option 53) ---
#define DHCP_DISCOVER       1
#define DHCP_OFFER          2
#define DHCP_REQUEST        3
#define DHCP_DECLINE        4
#define DHCP_ACK            5
#define DHCP_NAK            6
#define DHCP_RELEASE        7

// --- DNS Record Types ---
#define DNS_TYPE_A      1   // IPv4 Address
#define DNS_TYPE_NS     2   // Name Server
#define DNS_TYPE_CNAME  5   // Canonical Name
#define DNS_TYPE_SOA    6   // Start of Authority
#define DNS_TYPE_PTR    12  // Pointer (Reverse DNS)
#define DNS_TYPE_MX     15  // Mail Exchange
#define DNS_TYPE_TXT    16  // Text
#define DNS_TYPE_AAAA   28  // IPv6 Address

// --- DNS Classes ---
#define DNS_CLASS_IN    1   // Internet

// --- DNS Flags (Masks) ---
#define DNS_FLAG_QR     0x8000 // Query (0) / Response (1)
#define DNS_FLAG_OPCODE 0x7800 // Operation Code
#define DNS_FLAG_AA     0x0400 // Authoritative Answer
#define DNS_FLAG_TC     0x0200 // Truncated
#define DNS_FLAG_RD     0x0100 // Recursion Desired
#define DNS_FLAG_RA     0x0080 // Recursion Available
#define DNS_FLAG_RCODE  0x000F // Response Code (0=No Error, 3=Name Error/NXDOMAIN)

// --- Socket Level Options (setsockopt) ---
#define SOL_SOCKET      1
#define SO_REUSEADDR    2
#define SO_KEEPALIVE    9
#define SO_BROADCAST    6

// --- Error Codes (errno) ---
#define EPERM           1   // Operation not permitted
#define ENOENT          2   // No such file or directory
#define EIO             5   // I/O error
#define EBADF           9   // Bad file number
#define EAGAIN          11  // Try again (Non-blocking empty)
#define ENOMEM          12  // Out of memory
#define EACCES          13  // Permission denied
#define EFAULT          14  // Bad address
#define EINVAL          22  // Invalid argument
#define EPIPE           32  // Broken pipe
#define ENOTSOCK        88  // Socket operation on non-socket
#define EDESTADDRREQ    89  // Destination address required
#define EMSGSIZE        90  // Message too long
#define EPROTOTYPE      91  // Protocol wrong type for socket
#define ENOPROTOOPT     92  // Protocol not available
#define EPROTONOSUPPORT 93  // Protocol not supported
#define EAFNOSUPPORT    97  // Address family not supported by protocol
#define EADDRINUSE      98  // Address already in use
#define EADDRNOTAVAIL   99  // Cannot assign requested address
#define ENETDOWN        100 // Network is down
#define ENETUNREACH     101 // Network is unreachable
#define ECONNRESET      104 // Connection reset by peer
#define ENOBUFS         105 // No buffer space available
#define EISCONN         106 // Transport endpoint is already connected
#define ENOTCONN        107 // Transport endpoint is not connected
#define ETIMEDOUT       110 // Connection timed out
#define ECONNREFUSED    111 // Connection refused
#define EOPNOTSUPP     95  // Operation not supported on transport endpoint