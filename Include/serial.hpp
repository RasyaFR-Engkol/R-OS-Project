#pragma once

#include "rosval.h"
#include <port.hpp>

namespace Serial{
    void Init();
    void Write(const char *s);
    //char Read();
    void Printf(const char *fmt, ...);
    void VPrintf(const char *fmt, VA_LIST args);
    // Non-blocking: attempts to read one char from COM1; returns TRUE if a char was read
    BOOL TryReadChar(char *out);
    // Poll incoming serial input and mirror to FBConsole and Serial (with basic line-editing)
    VOID PollToConsoles();
    // Enable IRQ-driven input (unmask IRQ4 and register ISR). Call after PIC/IDT init.
    VOID EnableIRQInput();
    void SerialPutC(char c);

}