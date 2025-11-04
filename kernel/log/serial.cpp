#include "../Include/serial.hpp"
#include "../Include/rosval.h"
#include "fbcon/fbcon.hpp"
#include <logging.hpp>
#include <string.hpp>
#include <port.hpp>
#include "../driver/pic/pic.hpp"
#include "../intidt/idt.hpp"

namespace Serial {
    using namespace String;
    using namespace Port;

    void Init() {
        const uint16_t port = 0x3F8; // COM1
        Outb(port + 1, 0x00);    // Disable all interrupts
        Outb(port + 3, 0x80);    // Enable DLAB (set baud rate divisor)
        Outb(port + 0, 0x03);    // Divisor low byte (38400)
        Outb(port + 1, 0x00);    // Divisor high byte
        Outb(port + 3, 0x03);    // 8 bits, no parity, one stop bit
        // Enable Received Data Available Interrupt (IER bit0)
        Outb(port + 1, 0x01);
        Outb(port + 2, 0xC7);    // Enable FIFO, clear them, with 14-byte threshold
        Outb(port + 4, 0x0B);    // IRQs enabled, RTS/DSR set
    }
    
    void Write(const char* s) {
        for (const char* p = s; *p; ++p) SerialPutC(*p);
    }

    void Printf(const char *fmt, ...) {
        VA_LIST Args;
        VA_STRT(Args, fmt);

        // ketika masih ada karakter *fmt, kita akan masuk ke case
        // *fmt, lalu kita mulai iterasi 1 1 karakternya untuk di printf
        while(*fmt) {
            // Jika FMT ada persenan, maka itu adalah format, maka
            // kita harus masuk ke iterasi pemrosesan
            if(*fmt == '%') {
                // Skip persenan, naikan fmt
                fmt++;

                BOOL ZeroPadding = FALSE;
                // Kalo FMT formatting awalan punya %0 padding, maka
                // aktifkan padding
                if(*fmt == '0') {
                    ZeroPadding = TRUE;
                    fmt++;
                }

                // Kita Parse WIDTH nya
                VAL32 Width = 0;
                while(*fmt >= '0' && *fmt <= '9') {
                    Width = Width * 10 + (*fmt - '0');
                    fmt++;
                }

                // Parse length modifier: l, ll, z
                enum LenMod { LM_NONE, LM_L, LM_LL, LM_Z };
                LenMod LM = LM_NONE;
                if (*fmt == 'l') {
                    fmt++;
                    if (*fmt == 'l') { LM = LM_LL; fmt++; }
                    else LM = LM_L;
                } else if (*fmt == 'z') {
                    LM = LM_Z; fmt++;
                }

                CHAR8 Buf[64];
                const CHAR8 *str = NULL;

                switch(*fmt) {
                    case '%': {
                        Serial::SerialPutC('%');
                        break;
                    }
                    case 's': {
                        str = va_arg(Args, const CHAR8*);
                        if (!str) str = "(null)";
                        unsigned long long slen = Strlen(str);
                        int pad = (Width > (int)slen) ? (Width - (int)slen) : 0;
                        for (int i = 0; i < pad; ++i) Serial::SerialPutC(' ');
                        Serial::Write(str);
                        break;
                    }
                    case 'c': {
                        Serial::SerialPutC((char)va_arg(Args, int));
                        break;
                    }
                    case 'd':
                    case 'i': {
                        long long sval = 0;
                        if (LM == LM_LL) sval = va_arg(Args, long long);
                        else if (LM == LM_L) sval = (long long)va_arg(Args, long);
                        else if (LM == LM_Z) sval = (long long)va_arg(Args, size_t);
                        else sval = (long long)va_arg(Args, int);
                        Itoa(sval, Buf, 10);
                        CHAR8 *out = Buf;
                        bool neg = (out[0] == '-');
                        if (neg) ++out;
                        unsigned long long len = Strlen(out);
                        int total_len = (int)len + (neg ? 1 : 0);
                        if (ZeroPadding) {
                            if (neg) Serial::SerialPutC('-');
                            for (int i = total_len; i < Width; ++i) Serial::SerialPutC('0');
                            Serial::Write(out);
                        } else {
                            for (int i = total_len; i < Width; ++i) Serial::SerialPutC(' ');
                            if (neg) Serial::SerialPutC('-');
                            Serial::Write(out);
                        }
                        break;
                    }
                    case 'u': {
                        unsigned long long uval = 0ULL;
                        if (LM == LM_LL) uval = va_arg(Args, unsigned long long);
                        else if (LM == LM_L) uval = (unsigned long long)va_arg(Args, unsigned long);
                        else if (LM == LM_Z) uval = (unsigned long long)va_arg(Args, size_t);
                        else uval = (unsigned long long)va_arg(Args, unsigned int);
                        Utoa(uval, Buf, 10);
                        unsigned long long len = Strlen(Buf);
                        for (int i = (int)len; i < Width; ++i) Serial::SerialPutC(ZeroPadding ? '0' : ' ');
                        Serial::Write(Buf);
                        break;
                    }
                    case 'x':
                    case 'X': {
                        unsigned long long uval = 0ULL;
                        if (LM == LM_LL) uval = va_arg(Args, unsigned long long);
                        else if (LM == LM_L) uval = (unsigned long long)va_arg(Args, unsigned long);
                        else if (LM == LM_Z) uval = (unsigned long long)va_arg(Args, size_t);
                        else uval = (unsigned long long)va_arg(Args, unsigned int);
                        Utoa(uval, Buf, 16);
                        if (*fmt == 'X') {
                            // uppercase hex
                            for (char* p = Buf; *p; ++p) if (*p >= 'a' && *p <= 'f') *p = *p - 'a' + 'A';
                        }
                        unsigned long long len = Strlen(Buf);
                        for (int i = (int)len; i < Width; ++i) Serial::SerialPutC(ZeroPadding ? '0' : ' ');
                        Serial::Write(Buf);
                        break;
                    }
                    case 'p': {
                        void* ptr = va_arg(Args, void*);
                        unsigned long long pv = (unsigned long long)ptr;
                        Utoa(pv, Buf, 16);
                        Serial::SerialPutC('0'); Serial::SerialPutC('x');
                        Serial::Write(Buf);
                        break;
                    }
                    default: {
                        // Unknown specifier: print it literally
                        Serial::SerialPutC('%');
                        if (*fmt) Serial::SerialPutC(*fmt);
                        break;
                    }
                }
            } else {
                Serial::SerialPutC(*fmt);
            }
            fmt++;
        }
        VA_END(Args);
    }

    BOOL TryReadChar(char *out) {
        if (!out) return FALSE;
        const uint16_t port = 0x3F8; // COM1
        // LSR bit0: Data Ready
        if (Inb(port + 5) & 0x01) {
            *out = (char)Inb(port + 0);
            return TRUE;
        }
        return FALSE;
    }

    static inline void EchoCharToConsoles(char c) {
        if (c == '\r') {
            // Normalize CR to newline for fbcon, and CRLF on serial
            FBConsole::WriteString("\n");
            SerialPutC('\r');
            SerialPutC('\n');
        } else if (c == '\b' || c == 0x7f) {
            // Backspace on fbcon and serial
            FBConsole::WriteString("\b");
            SerialPutC('\b'); SerialPutC(' '); SerialPutC('\b');
        } else {
            char buf[2] = { c, 0 };
            FBConsole::WriteString(buf);
            SerialPutC(c);
        }
    }

    VOID PollToConsoles() {
        char ch;
        // Drain all currently available input
        while (TryReadChar(&ch)) {
            EchoCharToConsoles(ch);
        }
    }

    // IRQ handler for COM1 (IRQ4 -> remapped vector 0x20 + 4 = 0x24)
    static void Serial_OnIrq() {
        // Read and echo all available bytes
        char ch;
        const uint16_t port = 0x3F8;
        // While data ready
        while (Inb(port + 5) & 0x01) {
            ch = (char)Inb(port + 0);
            EchoCharToConsoles(ch);
        }
        // Acknowledge PIC for IRQ4
        PIC::SendEOI(4);
    }

    // Enable IRQ-driven serial input: unmask IRQ4 and register handler
    void EnableIRQInput() {
        // Unmask COM1 IRQ (IRQ4)
        PIC::EnableIRQ(4);
        // Register ISR at vector 0x20 + 4
        IDT::RegisterInterruptHandler((U8)(0x20 + 4), Serial_OnIrq);
        Printk::Write(Printk::Level::LOG_INFO, "[SERIAL] IRQ-driven input enabled on IRQ4 (vector 0x%02x)\n", (unsigned)(0x20 + 4));
    }

} // namespace Serial