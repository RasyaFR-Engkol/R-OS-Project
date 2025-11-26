#pragma once

#include <rosval.h>

namespace FBConsole {
    // Minimal API used by Printk to mirror output to framebuffer console.
    // fbcon.cpp provides implementations for these functions.
    BOOL IsReady();
    VOID WriteString(const CHAR8 *s);
    VOID UpdateCursor();
    U64 GetColumns();
    U64 GetRows();
    // Compatibility helpers used by TTY ioctl
    U64 GetCols();
    U64 GetWidthPixels();
    U64 GetHeightPixels();
}
