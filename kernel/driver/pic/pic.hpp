#pragma once
#include "rosval.h"
#include <task.hpp>

// Forward-declare Task type in case inclusion order differs
namespace Tasking { struct Task; }

// Port PIC 1
#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21

// Port PIC 2
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

namespace PIC {
    extern BOOL G_StillLegacyINTx;
    void InitializePIC();
    void Remap(U8 offset1, U8 offset2);
    void SendEOI(U8 irq);
    void EnableIRQ(U8 irq);
    namespace Keyboard{
        void InitializeKeyboardPIC();
        // Poll queued scancodes (call from main loop / console task)
        void Poll();
        
        // Blocking read for Stdin
        char GetChar();

        U32 GetBufferCount();
        // Clear input ASCII buffer (used by TCSETSF)
        void FlushBuffer();
        void NotifyTaskDied(Tasking::Task*);
        void InjectScancode(U8 sc);
    }
    VOID DisableIRQWhileAndMaskOldPIC();
}