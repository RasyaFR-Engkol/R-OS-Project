#pragma once
#include "rosval.h"

namespace PIC {
    void InitializePIC();
    void Remap(U8 offset1, U8 offset2);
    void SendEOI(U8 irq);
    void EnableIRQ(U8 irq);
    namespace Keyboard{
        void InitializeKeyboardPIC();
        // Poll queued scancodes (call from main loop / console task)
        void Poll();
        
    }
}