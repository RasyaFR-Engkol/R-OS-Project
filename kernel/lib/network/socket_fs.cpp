#include "rosval.h"
#include <network/socket_fs.hpp>
#include <network/tcp.hpp>
#include <network/udp.hpp>
#include <network/ethernet.hpp>
#include <mm.hpp>

// Definisi Instance Global
SocketFileSystem* SocketFileSystem::Instance = nullptr;

SocketFileSystem* SocketFileSystem::GetInstance() {
    if (!Instance) {
        Instance = new SocketFileSystem();
    }
    return Instance;
}

// Read: Panggil TCPRecv / UDPRecv / ICMPRecv
U32 SocketFileSystem::Read(File* file, U8* Buffer, U32 Size) {
    SocketFile* sockFile = (SocketFile*)file;
    if (sockFile->Type == SocketFile::TCP) {
        return Network::TCPRecv(sockFile->Handle.Tcp, Buffer, Size);
    } else if (sockFile->Type == SocketFile::UDP) {
        return Network::UDPRecv(sockFile->Handle.Udp, Buffer, Size);
    } else if (sockFile->Type == SocketFile::ICMP) {
        return Network::ICMPRecv(sockFile->Handle.Icmp, Buffer, Size);
    }
    return 0;
}

// Write: Panggil SendTCPData / SendUDP / SendICMP
U32 SocketFileSystem::Write(File* file, U8* Buffer, U32 Size) {
    SocketFile* sockFile = (SocketFile*)file;
    if (sockFile->Type == SocketFile::TCP) {
        Printk::Write(Printk::Level::LOG_DEBUG, "SocketFileSystem::Write: Sending %u bytes on TCP socket\n", Size);
        Network::SendTCPData(Network::E1000::g_E1000Instance, sockFile->Handle.Tcp, Buffer, Size);
        return Size; 
    } else if (sockFile->Type == SocketFile::UDP) {
        // UDP butuh target IP/Port, biasanya pake sendto. 
        // Untuk write() biasa, kita asumsikan sudah connect() (set default remote).
        Printk::Write(Printk::Level::LOG_DEBUG, "SocketFileSystem::Write: Sending %u bytes on UDP socket\n", Size);
        Network::UDPSocket* uSock = sockFile->Handle.Udp;
        if (uSock->RemoteIP != 0 && uSock->RemotePort != 0) {
            Network::SendUDP(Network::E1000::g_E1000Instance, uSock->RemoteIP, uSock->LocalPort, uSock->RemotePort, Buffer, Size);
            return Size;
        }
        return 0; // Destination not set
    } else if (sockFile->Type == SocketFile::ICMP) {
        Printk::Write(Printk::Level::LOG_DEBUG, "SocketFileSystem::Write: Sending %u bytes on ICMP socket\n", Size);
        Network::ICMPSocket* iSock = sockFile->Handle.Icmp;
        iSock->Sequence++;
        Network::SendICMP(Network::E1000::g_E1000Instance, iSock->RemoteIP, iSock->Sequence, iSock->ID, Buffer, Size);
        return Size;
    }
    return 0;
}

void SocketFileSystem::Close(File* file) {
    SocketFile* sockFile = (SocketFile*)file;
    if (sockFile->Type == SocketFile::TCP) {
        Network::FreeSocket(sockFile->Handle.Tcp);
    } else if (sockFile->Type == SocketFile::UDP) {
        Network::UDPClose(sockFile->Handle.Udp);
    } else if (sockFile->Type == SocketFile::ICMP) {
        Network::ICMPClose(sockFile->Handle.Icmp);
    }
    delete sockFile;
}

U64 SocketFileSystem::CreateNode(const char* path, U32 Flags) {
    Printk::Write(Printk::Level::LOG_DEBUG, "SocketFileSystem::CreateNode: Creating socket with path '%s' and flags 0x%X\n", path, Flags);
    Printk::Write(Printk::Level::LOG_DEBUG, "UNIMPLEMENTED\n");
    return 0;
}