#pragma once

#include <rosval.h>

namespace Network{
    STATIC INLINE U16 CalculateChecksum(VOID* data, INTN length) {
        U16* ptr = (U16*)data;
        U32 sum = 0;
        
        while (length > 1) {
            sum += *ptr++;
            length -= 2;
        }
        if (length > 0) {
            sum += *(U8*)ptr;
        }
        
        while (sum >> 16) {
            sum = (sum & 0xFFFF) + (sum >> 16);
        }
        
        return (U16)(~sum);
    }
}