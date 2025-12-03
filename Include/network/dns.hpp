#pragma once
#include <rosval.h>
#include <network/netinterface.hpp>

namespace Network{
    struct DNSHeader {
        U16 ID;          // Identifikasi Query (biar tau reply-nya punya siapa)
        U16 Flags;       // Standard Query, Recursion Desired, dll
        U16 QuestionCount;
        U16 AnswerCount;
        U16 AuthorityCount;
        U16 AdditionalCount;
    } PACKSTRUCT;

    VOID SendDNSQuery(NetworkInterface *Card, const char *DomainName);
    VOID HandleDNS(NetworkInterface *Card, U8 *Data, U32 Length);
}