#pragma once

#include <rosval.h>
#include "inputd.hpp"

class InputManager{
    private:
        STATIC CONSTANT INTN BufferSize = 256;
        STATIC INLINE InputEvent EventBuffer[BufferSize];
        STATIC INLINE VOLATILE INTN ReadIdx;
        STATIC INLINE VOLATILE INTN WriteIdx;
    public:
        STATIC VOID PushEvent(InputEvent ev){
            INTN NextWrite = (WriteIdx + 1) % BufferSize;

            if(NextWrite != ReadIdx){
                EventBuffer[WriteIdx] = ev;
                WriteIdx = NextWrite;   
            }
        }

        STATIC BOOL PopEvent(InputEvent &outEv){
            if(ReadIdx == WriteIdx) return FALSE;

            outEv = EventBuffer[ReadIdx];
            ReadIdx = (ReadIdx + 1) % BufferSize;
            return TRUE;
        }
};