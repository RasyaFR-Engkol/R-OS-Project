#pragma once

#include <rosval.h>

namespace Network{
    // Host to Network Short (dan sebaliknya)
    inline U16 Swap16(U16 val) {
        return (val << 8) | (val >> 8);
    }

    // Host to Network Long (dan sebaliknya)
    inline U32 Swap32(U32 val) {
        return ((val << 24) & 0xFF000000) |
               ((val <<  8) & 0x00FF0000) |
               ((val >>  8) & 0x0000FF00) |
               ((val >> 24) & 0x000000FF);
    }
    
    #define ntohs(x) Swap16(x)
    #define htons(x) Swap16(x)
    #define ntohl(x) Swap32(x)
    #define htonl(x) Swap32(x)
}