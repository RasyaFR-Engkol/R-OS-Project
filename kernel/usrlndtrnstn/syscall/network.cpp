#include "network/macro.hpp"
#include "rossys.hpp"
#include <rosval.h>
#include <cpu_context.hpp>
#include <task.hpp>
#include <network/tcp.hpp>
#include <network/udp.hpp>
#include <network/socket_fs.hpp> // Include wrapper tadi
#include <network/swapper.hpp>
#include <rng/entrophy.hpp>

// Syscall: socket(domain, type, protocol)
// AF_INET = 2, SOCK_STREAM = 1 (TCP), SOCK_DGRAM = 2 (UDP)
VOID Sys_Socket(CpuContext_T *CPUContext){
    Printk::Write(Printk::Level::LOG_DEBUG, "Sys_Socket: Called\n");
    U64 Domain = CPUContext->rdi;
    U64 type = CPUContext->rsi;
    // U64 Protocol = CPUContext->rdx;

    Tasking::Task *CurTask = Tasking::GetCurrentTaskPtr();
    if(!CurTask){
        CPUContext->rax = (U64)(-1); // Error
        return;
    }

    if(Domain != AF_INET){
        Printk::Write(Printk::Level::LOG_DEBUG, "Sys_Socket: Unsupported domain %llu\n", (unsigned long long)Domain);
        CPUContext->rax = -EAFNOSUPPORT; // Error: Unsupported domain
        return;
    }

    INTN fd = -1;
    for(INTN i = 0; i < MAX_FILE_IN_PROCESS; i++){
        if(CurTask->FDTable[i] == nullptr){
            fd = i;
            break;
        }
    }
    if(fd == -1){
        Printk::Write(Printk::Level::LOG_DEBUG, "Sys_Socket: FD Table full for PID %llu\n", (unsigned long long)CurTask->pid);
        CPUContext->rax = (U64)(-1); // Error: FD Table penuh
        return;
    }

    SocketFile* sockFile = new SocketFile();
    sockFile->Node->FSOwner = SocketFileSystem::GetInstance();

    if (type == SOCK_STREAM) { // TCP
        Printk::Write(Printk::Level::LOG_DEBUG, "Sys_Socket: Creating TCP socket\n");
        sockFile->Type = SocketFile::TCP;
        sockFile->Handle.Tcp = Network::AllocateSocket(); // Panggil Kernel TCP
        if (!sockFile->Handle.Tcp) {
            delete sockFile;
            CPUContext->rax = -ENOMEM;
            return;
        }
    } else if (type == SOCK_DGRAM) { // UDP
        Printk::Write(Printk::Level::LOG_DEBUG, "Sys_Socket: Creating UDP socket\n");
        sockFile->Type = SocketFile::UDP;
        sockFile->Handle.Udp = Network::AllocateUDPSocket(); // Panggil Kernel UDP
         if (!sockFile->Handle.Udp) {
            delete sockFile;
            CPUContext->rax = -ENOMEM;
            return;
        }
    } else if (type == SOCK_RAW) { // Biasanya ping pake SOCK_RAW
        Printk::Write(Printk::Level::LOG_DEBUG, "Sys_Socket: Creating ICMP socket\n");
        sockFile->Type = SocketFile::ICMP;
        
        // Generate ID unik (misal pake Random atau PID task)
        U16 PingID = EntrophySystem::Random() & 0xFFFF;
        
        sockFile->Handle.Icmp = Network::ICMPOpen(PingID);
        if (!sockFile->Handle.Icmp) {
            delete sockFile;
            CPUContext->rax = -ENOMEM;
            return;
        }
        
        // Init default
        sockFile->Handle.Icmp->RemoteIP = 0;
        sockFile->Handle.Icmp->Sequence = 0;
    }  else {
        Printk::Write(Printk::Level::LOG_DEBUG, "Sys_Socket: Unsupported socket type %llu\n", (unsigned long long)type);
        delete sockFile;
        CPUContext->rax = -EINVAL; // Unknown type
        return;
    }

    CurTask->FDTable[fd] = sockFile;
    CPUContext->rax = fd;
}

// Syscall: connect(fd, sockaddr*, len)
// Struct sockaddr_in { short family; short port; int addr; char zero[8]; };
VOID Sys_Connect(CpuContext_T *CPUContext) {
    U64 fd = CPUContext->rdi;
    U64 addr_ptr = CPUContext->rsi;
    // U64 len = CPUContext->rdx;

    Tasking::Task *Curtask = Tasking::GetCurrentTaskPtr();
    if (!Curtask || fd >= MAX_FILE_IN_PROCESS || !Curtask->FDTable[fd]) {
        Printk::Write(Printk::Level::LOG_DEBUG, "Sys_Connect: Invalid task or fd %llu\n", (unsigned long long)fd);
        CPUContext->rax = -1; return;
    }

    if (!Curtask->FDTable[fd]) {
        Printk::Write(Printk::Level::LOG_DEBUG, "Sys_Connect: FD %llu is not open\n", (unsigned long long)fd);
        CPUContext->rax = -EBADF;
        return;
    }

    // Ambil struct sockaddr dari userland
    struct {
        U16 family;
        U16 port; // Big Endian
        U32 addr; // Big Endian
    } Addr;

    U64 *user_pml4 = HHDM_PhysToVirt(Curtask->CR3);
    if (!PageAlloc::CopyFromUser(user_pml4, &Addr, (void*)addr_ptr, sizeof(Addr))) {
        Printk::Write(Printk::Level::LOG_DEBUG, "Sys_Connect: CopyFromUser failed for sockaddr\n");
        CPUContext->rax = -1; return;
    }

    if (Addr.family != AF_INET) {
        Printk::Write(Printk::Level::LOG_DEBUG, "Sys_Connect: Unsupported address family %u\n", (unsigned)Addr.family);
        CPUContext->rax = -EAFNOSUPPORT;
        return;
    }

    SocketFile* sockFile = (SocketFile*)Curtask->FDTable[fd];
    
    // Pastikan ini beneran socket, bukan file biasa!
    if (sockFile->Node->FSOwner != SocketFileSystem::GetInstance()) {
        Printk::Write(Printk::Level::LOG_DEBUG, "Sys_Connect: FD %llu is not a socket\n", (unsigned long long)fd);
        CPUContext->rax = -1; // ENOTSOCK
        return;
    }

    if (sockFile->Type == SocketFile::TCP) {
        Printk::Write(Printk::Level::LOG_DEBUG, "Sys_Connect: Connecting TCP socket to %d.%d.%d.%d:%u\n",
            (Addr.addr & 0xFF), (Addr.addr >> 8) & 0xFF, (Addr.addr >> 16) & 0xFF, (Addr.addr >> 24) & 0xFF,
            Network::ntohs(Addr.port));
        // Panggil Kernel TCP Connect
        // Note: TCPConnect kamu return Socket*, tapi kita udah punya Socket* dari AllocateSocket.
        // Jadi kita harus set manual parameternya dan kirim SYN.
        
        Network::TCPSocket* kSock = sockFile->Handle.Tcp;

        if (kSock->State != Network::CLOSED && kSock->State != Network::LISTEN) {
             CPUContext->rax = -EISCONN; // Already connected
             return;
        }

        kSock->RemoteIP = Addr.addr; // IP dari user (Network Order)
        kSock->RemotePort = Network::ntohs(Addr.port); // Port dari user (Network Order -> Host Order)
        kSock->LocalIP = Network::E1000::g_E1000Instance->IP;
        kSock->LocalPort = 40000 + kSock->ID; // Dynamic Port
        
        // Start Handshake
        kSock->MySequence = EntrophySystem::Random();
        Network::SendTCPRaw(Network::E1000::g_E1000Instance, kSock, TCP_SYN, nullptr, 0);
        kSock->State = Network::SYN_SENT;

        // BLOCKING WAIT (Simple)
        // Di OS beneran, task harusnya di-block (Sleep) sampai state berubah jadi ESTABLISHED.
        // Untuk sekarang, kita spin-wait sebentar atau return success (Non-blocking connect).
        // Idealnya: SchedulerBlock(Curtask);
        // 
        // Tapi interrupt handler EthernetStack belom bangunin task yang tidur
        // akibat nunggu task. Jadi kita pake cara simple dulu.
        // Spin-wait dengan timeout 500ms
        Arch::Time::SleepMs(500);
        
        if (kSock->State == Network::ESTABLISHED) {
            CPUContext->rax = 0; // Success
        } else {
            // Timeout atau Refused
            CPUContext->rax = -ETIMEDOUT; 
        }
    } else if (sockFile->Type == SocketFile::UDP) {
        Printk::Write(Printk::Level::LOG_DEBUG, "Sys_Connect: Connecting UDP socket to %d.%d.%d.%d:%u\n",
            (Addr.addr & 0xFF), (Addr.addr >> 8) & 0xFF, (Addr.addr >> 16) & 0xFF, (Addr.addr >> 24) & 0xFF,
            Network::ntohs(Addr.port));
        // UDP "Connect" cuma set default destination
        Network::UDPSocket* kSock = sockFile->Handle.Udp;
        kSock->RemoteIP = Addr.addr;
        kSock->RemotePort = Network::ntohs(Addr.port);
        CPUContext->rax = 0;
    } else if (sockFile->Type == SocketFile::ICMP) {
        Printk::Write(Printk::Level::LOG_DEBUG, "Sys_Connect: Connecting ICMP socket to %d.%d.%d.%d\n",
            (Addr.addr & 0xFF), (Addr.addr >> 8) & 0xFF, (Addr.addr >> 16) & 0xFF, (Addr.addr >> 24) & 0xFF);
        // Untuk ICMP, connect() cuma buat set tujuan default
        Network::ICMPSocket* kSock = sockFile->Handle.Icmp;
        kSock->RemoteIP = Addr.addr;
        CPUContext->rax = 0;
    } else {
        Printk::Write(Printk::Level::LOG_DEBUG, "Sys_Connect: Unknown socket type %d\n", sockFile->Type);
        CPUContext->rax = -1; // ENOTSOCK
        return;
    }
}