#include "network/tcp.hpp"
#include <network/dhcp.hpp>
#include <network/netinterface.hpp>
#include <network/ethernet.hpp>
#include <network/ipv4.hpp>
#include <network/swapper.hpp>
#include <rosval.h>
#include <string.hpp>
#include <mm.hpp>
#define PRINTK_MODULE_NAME "DNS"
#include <logging.hpp>
#include <network/dns.hpp>

namespace Network{
    INTN EncodeDNSName(U8 *Buffer, const char *Domain){
        INTN len = String::Strlen(Domain);

        INTN Lock = 0;

        String::Strcpy((char*)Buffer + 1, Domain);

        for(INTN i = 0; i < len; i++){
            if(Buffer[i + 1] == '.'){
                Buffer[Lock] = i - Lock;
                Lock = i + 1;
            }
        }

        Buffer[Lock] = len - Lock;

        Buffer[len + 1] = 0;

        return len + 2;
    }

    VOID SendDNSQuery(NetworkInterface *Card, const char *DomainName){
        if(Card->DnsServer == 0){
            Printk::Write(Printk::Level::LOG_WARNING, "[DNS] No DNS Server configured via DHCP!\n");
            return;
        }

        // Alokasi Buffer UDP Payload
        // Header (12) + Max Domain (256) + Type(2) + Class(2)
        U8* Buffer = (U8*)Kmalloc::Alloc(512); 
        DNSHeader* Header = (DNSHeader*)Buffer;

        // 1. Isi Header
        Header->ID = htons(0xCAFE); // ID Bebas
        Header->Flags = htons(0x0100); // Standard Query, Recursion Desired
        Header->QuestionCount = htons(1); // 1 Pertanyaan
        Header->AnswerCount = 0;
        Header->AuthorityCount = 0;
        Header->AdditionalCount = 0;

        // 2. Encode DNS Name
        U8 *QuestionPTR = (U8*)(Header + 1);
        INTN NameLen = EncodeDNSName(QuestionPTR, DomainName);

        QuestionPTR += NameLen;

        *(U16*)QuestionPTR = htons(1); 
        QuestionPTR += 2;

        *(U16*)QuestionPTR = htons(1);
        QuestionPTR += 2;

        U32 TotalLen = (U32)(QuestionPTR - Buffer);

        Printk::Write(Printk::Level::LOG_INFO, "[DNS] Querying %s at %x...\n", DomainName, Card->DnsServer);

        SendUDP(Card, Card->DnsServer, 55555, 53, Buffer, TotalLen);

        Kmalloc::Free(Buffer);
    }

    U8* SkipDNSName(U8* Ptr, U8* StartOfPacket) {
        while (*Ptr != 0) {
            // Cek Compression Flag (0xC0)
            if ((*Ptr & 0xC0) == 0xC0) {
                // Compression: 2 bytes pointer (0xC0xx)
                // Kita cuma perlu skip 2 byte ini, karena ini akhir dari nama.
                return Ptr + 2; 
            }
            // Label biasa: [Len][Text...]
            U8 Len = *Ptr;
            Ptr += (Len + 1);
        }
        return Ptr + 1; // Skip null terminator (0x00)
    }

    U8* DecodeDNSName(U8* Ptr, U8* StartOfPacket, char* OutBuffer) {
        int OutIdx = 0;
        BOOL Jumped = FALSE;
        U8* ReturnPtr = nullptr;
        int Jumps = 0;

        while (*Ptr != 0 && Jumps < 10) { // Limit jumps to prevent infinite loops
            if ((*Ptr & 0xC0) == 0xC0) {
                if (!Jumped) {
                    ReturnPtr = Ptr + 2;
                    Jumped = TRUE;
                }
                U16 Offset = ntohs(*(U16*)Ptr) & 0x3FFF;
                Ptr = StartOfPacket + Offset;
                Jumps++;
            } else {
                U8 Len = *Ptr++;
                for (int i = 0; i < Len; i++) {
                    if(OutIdx < 255) OutBuffer[OutIdx++] = *Ptr++;
                    else Ptr++;
                }
                if(OutIdx < 255) OutBuffer[OutIdx++] = '.';
            }
        }
        
        if (OutIdx > 0) OutBuffer[OutIdx - 1] = 0; // Remove trailing dot
        else OutBuffer[0] = 0;

        if (!Jumped) return Ptr + 1;
        return ReturnPtr;
    }

    VOID HandleDNS(NetworkInterface *Card, U8 *Data, U32 Length){
        if (Length < sizeof(DNSHeader)) return;

        DNSHeader* Header = (DNSHeader*)Data;
        
        // Cek ID (Harus 0xCAFE sesuai request kita)
        if (ntohs(Header->ID) != 0xCAFE) return;
        
        // Cek Answer Count (Ada jawaban gak?)
        U16 Answers = ntohs(Header->AnswerCount);
        if (Answers == 0) {
            Printk::Write(Printk::Level::LOG_WARNING, "[DNS] No answers received.\n");
            return;
        }

        // Pointer mulai setelah Header
        U8* Ptr = (U8*)(Header + 1);
        char ResolvedDomain[256];
        ResolvedDomain[0] = 0;

        // 1. SKIP QUESTION SECTION
        // Kita harus lewati bagian "Question" dulu buat nyampe ke "Answer"
        U16 Questions = ntohs(Header->QuestionCount);
        for (int i = 0; i < Questions; i++) {
            if (i == 0) {
                Ptr = DecodeDNSName(Ptr, Data, ResolvedDomain);
            } else {
                Ptr = SkipDNSName(Ptr, Data); // Lewati Nama Domain
            }
            Ptr += 4; // Lewati Type(2) + Class(2)
        }

        // 2. PARSE ANSWERS
        for (int i = 0; i < Answers; i++) {
            // -- Answer Name --
            Ptr = SkipDNSName(Ptr, Data); 
            
            // -- Data Answer --
            U16 Type = ntohs(*(U16*)Ptr); Ptr += 2;
            U16 Class = ntohs(*(U16*)Ptr); Ptr += 2;
            __MAYBE_UNUSED U32 TTL = ntohl(*(U32*)Ptr); Ptr += 4; // Time To Live
            U16 DataLen = ntohs(*(U16*)Ptr); Ptr += 2;

            // Kita cuma cari Type A (IPv4) = 1 dan Class IN = 1
            if (Type == 1 && Class == 1 && DataLen == 4) {
                U32 ResolvedIP;
                String::Memcpy(&ResolvedIP, Ptr, 4);
                
                U8* IPBytes = (U8*)&ResolvedIP;
                Printk::Write(Printk::Level::LOG_INFO, "[DNS] Resolved %s! IP: %d.%d.%d.%d\n", 
                    ResolvedDomain, IPBytes[0], IPBytes[1], IPBytes[2], IPBytes[3]);

                // --- GONG-NYA DISINI ---
                // PING HASIL RESOLVE TADI!
                // Pastikan variabel 'Seq' atau 'ID' statis atau naik terus biar keren
                //Nework::SendICMP(Card, ResolvedIP, 1, 0x1234, (U8*)"Hello Google from DNS", 21);

                Printk::Write(Printk::Level::LOG_INFO, "[KERNEL] Connecting via TCP to Target...\n");
                
                // Pake TCPConnect. Dia bakal alokasi socket baru dan kirim SYN.
                TCPSocket* HttpSock = Network::TCPConnect(Card, ResolvedIP, 80, ResolvedDomain);
                
                if (HttpSock) {
                     Printk::Write(Printk::Level::LOG_INFO, "[DNS] Socket %d allocated via DHCP trigger!\n", HttpSock->ID);
                } else {
                     Printk::Write(Printk::Level::LOG_ERR, "[DNS] Failed to allocate TCP Socket (Table Full)\n");
                }
                
                return; // Kita ambil jawaban pertama aja, cukup.
            }

            // Lanjut ke answer berikutnya
            Ptr += DataLen;
        }
    }
}