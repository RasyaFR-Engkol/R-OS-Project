#pragma once

#include "rosval.h"
#include <port.hpp>

namespace Serial{
    extern BOOL BLOCK;
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

#ifndef COLORS_H
#define COLORS_H

// --- KONTROL DASAR ---
#define ANSI_RESET          "\x1b[0m"
#define ANSI_BOLD           "\x1b[1m"
#define ANSI_DIM            "\x1b[2m"
#define ANSI_ITALIC         "\x1b[3m"
#define ANSI_UNDERLINE      "\x1b[4m"
#define ANSI_BLINK          "\x1b[5m"
#define ANSI_REVERSE        "\x1b[7m"
#define ANSI_HIDDEN         "\x1b[8m"

// --- WARNA TEXT (FOREGROUND) ---
#define ANSI_FG_BLACK       "\x1b[30m"
#define ANSI_FG_RED         "\x1b[31m"
#define ANSI_FG_GREEN       "\x1b[32m"
#define ANSI_FG_YELLOW      "\x1b[33m"
#define ANSI_FG_BLUE        "\x1b[34m"
#define ANSI_FG_MAGENTA     "\x1b[35m"
#define ANSI_FG_CYAN        "\x1b[36m"
#define ANSI_FG_WHITE       "\x1b[37m"

// --- WARNA BACKGROUND ---
#define ANSI_BG_BLACK       "\x1b[40m"
#define ANSI_BG_RED         "\x1b[41m"
#define ANSI_BG_GREEN       "\x1b[42m"
#define ANSI_BG_YELLOW      "\x1b[43m"
#define ANSI_BG_BLUE        "\x1b[44m"
#define ANSI_BG_MAGENTA     "\x1b[45m"
#define ANSI_BG_CYAN        "\x1b[46m"
#define ANSI_BG_WHITE       "\x1b[47m"

// --- WARNA TEXT TERANG (BRIGHT FOREGROUND) ---
#define ANSI_FG_BRIGHT_BLACK    "\x1b[90m"  // Abu-abu
#define ANSI_FG_BRIGHT_RED      "\x1b[91m"
#define ANSI_FG_BRIGHT_GREEN    "\x1b[92m"
#define ANSI_FG_BRIGHT_YELLOW   "\x1b[93m"
#define ANSI_FG_BRIGHT_BLUE     "\x1b[94m"
#define ANSI_FG_BRIGHT_MAGENTA  "\x1b[95m"
#define ANSI_FG_BRIGHT_CYAN     "\x1b[96m"
#define ANSI_FG_BRIGHT_WHITE    "\x1b[97m"

#endif // COLORS_H