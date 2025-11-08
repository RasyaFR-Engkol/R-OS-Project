#pragma once

#include <rosval.h>

namespace FBConsole {
    // Minimal API used by Printk to mirror output to framebuffer console.
    // fbcon.cpp provides implementations for these functions.
    BOOL IsReady();
    VOID WriteString(const CHAR8 *s);
    VOID UpdateCursor();
}
