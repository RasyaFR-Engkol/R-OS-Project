#define PRINTK_MODULE_NAME "SERIAL"
#include "../Include/serial.hpp"
#include "../Include/rosval.h"
#include "fbcon/fbcon.hpp"
#include <logging.hpp>
#include <string.hpp>
#include <port.hpp>
#include "../driver/pic/pic.hpp"
#include "../intidt/idt.hpp"
#include <task.hpp>

/* module name provided via PRINTK_MODULE_NAME */

namespace Serial {
    using namespace String;
    using namespace Port;

    // IRQ-driven RX buffer (single-producer (IRQ) / single-consumer (non-IRQ))
    static constexpr unsigned int RX_BUF_SIZE = 1024u;
    static volatile unsigned int s_rx_head = 0; // next write index
    static volatile unsigned int s_rx_tail = 0; // next read index
    static char s_rx_buf[RX_BUF_SIZE];
    BOOL BLOCK = FALSE;

    void SerialPutC(char c) {
        const U16 port = 0x3F8;
        // Wait for Transmitter Holding Register empty
        // Add a timeout so we don't spin forever if the UART/IO is stuck.
        // Without this, a hung serial TX bit can cause the kernel to appear
        // to freeze because string output (from IRQ or normal context)
        // busy-waits indefinitely.
        const unsigned int timeout_max = 100000u;
        unsigned int timeout = timeout_max;
        while (!(Port::Inb(port + 5) & 0x20)) {
            if (--timeout == 0u) return;

            // REMOVED: Tasking::SchedulerYield();
            // Yielding inside SerialPutC is dangerous because this function
            // is called from IRQ handlers (Keyboard, Serial RX).
            // Context switching away from an ISR prevents EOI from being sent,
            // blocking further interrupts and causing massive lag.
            // Just spin-wait here.
            asm volatile("pause");
        }
        Port::Outb(port, (U8)c);
    }

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
        if(Serial::BLOCK){
            return;
        }
        for (const char* p = s; *p; ++p) SerialPutC(*p);
    }

    void VPrintf(const char *fmt, VA_LIST args) {
        // Iterate over format string, consuming arguments from `args`
        while(*fmt) {
            if(*fmt == '%') {
                fmt++;

                BOOL ZeroPadding = FALSE;
                if(*fmt == '0') {
                    ZeroPadding = TRUE;
                    fmt++;
                }

                VAL32 Width = 0;
                while(*fmt >= '0' && *fmt <= '9') {
                    Width = Width * 10 + (*fmt - '0');
                    fmt++;
                }

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
                        str = va_arg(args, const CHAR8*);
                        if (!str) str = "(null)";
                        unsigned long long slen = Strlen(str);
                        int pad = (Width > (int)slen) ? (Width - (int)slen) : 0;
                        for (int i = 0; i < pad; ++i) Serial::SerialPutC(' ');
                        Serial::Write(str);
                        break;
                    }
                    case 'c': {
                        Serial::SerialPutC((char)va_arg(args, int));
                        break;
                    }
                    case 'd':
                    case 'i': {
                        long long sval = 0;
                        if (LM == LM_LL) sval = va_arg(args, long long);
                        else if (LM == LM_L) sval = (long long)va_arg(args, long);
                        else if (LM == LM_Z) sval = (long long)va_arg(args, size_t);
                        else sval = (long long)va_arg(args, int);
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
                        if (LM == LM_LL) uval = va_arg(args, unsigned long long);
                        else if (LM == LM_L) uval = (unsigned long long)va_arg(args, unsigned long);
                        else if (LM == LM_Z) uval = (unsigned long long)va_arg(args, size_t);
                        else uval = (unsigned long long)va_arg(args, unsigned int);
                        Utoa(uval, Buf, 10);
                        unsigned long long len = Strlen(Buf);
                        for (int i = (int)len; i < Width; ++i) Serial::SerialPutC(ZeroPadding ? '0' : ' ');
                        Serial::Write(Buf);
                        break;
                    }
                    case 'x':
                    case 'X': {
                        unsigned long long uval = 0ULL;
                        if (LM == LM_LL) uval = va_arg(args, unsigned long long);
                        else if (LM == LM_L) uval = (unsigned long long)va_arg(args, unsigned long);
                        else if (LM == LM_Z) uval = (unsigned long long)va_arg(args, size_t);
                        else uval = (unsigned long long)va_arg(args, unsigned int);
                        Utoa(uval, Buf, 16);
                        if (*fmt == 'X') {
                            for (char* p = Buf; *p; ++p) if (*p >= 'a' && *p <= 'f') *p = *p - 'a' + 'A';
                        }
                        unsigned long long len = Strlen(Buf);
                        for (int i = (int)len; i < Width; ++i) Serial::SerialPutC(ZeroPadding ? '0' : ' ');
                        Serial::Write(Buf);
                        break;
                    }
                    case 'p': {
                        void* ptr = va_arg(args, void*);
                        unsigned long long pv = (unsigned long long)ptr;
                        Utoa(pv, Buf, 16);
                        Serial::SerialPutC('0'); Serial::SerialPutC('x');
                        Serial::Write(Buf);
                        break;
                    }
                    default: {
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
    }

    void Printf(const char *fmt, ...) {
        if(Serial::BLOCK){
            return;
        }
        VA_LIST Args;
        VA_STRT(Args, fmt);
        VPrintf(fmt, Args);
        VA_END(Args);
    }

    BOOL TryReadChar(char *out) {
        if (!out) return FALSE;
        // Try to read from IRQ-driven RX buffer first
        extern volatile unsigned int s_rx_head;
        extern volatile unsigned int s_rx_tail;
        extern char s_rx_buf[];
        const unsigned int head = s_rx_head;
        const unsigned int tail = s_rx_tail;
        if (tail != head) {
            // buffer implementation stores data at indexes 0..RX_BUF_SIZE-1
            unsigned int idx = tail % RX_BUF_SIZE;
            *out = s_rx_buf[idx];
            s_rx_tail = (tail + 1) % RX_BUF_SIZE;
            return TRUE;
        }

        // Fallback: poll hardware directly
        const uint16_t port = 0x3F8; // COM1
        if (Inb(port + 5) & 0x01) {
            *out = (char)Inb(port + 0);
            return TRUE;
        }
        return FALSE;
    }

    static inline void EchoCharToConsoles(char c) {
        return; 
        // sudah di ECHO tty. jadi ga perlu echo.
        // return dari sini biar ga double echo 
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
        return;
        char ch;
        // Drain IRQ-driven RX buffer and then poll hardware
        while (TryReadChar(&ch)) {
            EchoCharToConsoles(ch);
        }
    }

    // IRQ handler for COM1 (IRQ4 -> remapped vector 0x20 + 4 = 0x24)
    static void Serial_OnIrq(void *Context = nullptr) {
        // Minimal IRQ handler: read bytes from hardware and push into
        // a small lock-free circular buffer. Do NOT call printing or
        // other heavy subsystems from IRQ context.
        const uint16_t port = 0x3F8;
        while (Inb(port + 5) & 0x01) {
            char ch = (char)Inb(port + 0);

            // push into buffer
            unsigned int head = s_rx_head;
            unsigned int next = (head + 1) % RX_BUF_SIZE;
            if (next == s_rx_tail) {
                // buffer full, drop char
                continue;
            }
            s_rx_buf[head % RX_BUF_SIZE] = ch;
            s_rx_head = next;
        }
        // EOI is handled by the central IRQ dispatcher (IrqDispatch),
        // which sends either PIC or LAPIC EOI depending on controller.
    }

    // Enable IRQ-driven serial input: unmask IRQ4 and register handler
    void EnableIRQInput() {
        // Unmask COM1 IRQ (IRQ4)
        PIC::EnableIRQ(4);
        // Register ISR at vector 0x20 + 4
        IDT::RegisterInterruptHandler((U8)(0x20 + 4), Serial_OnIrq);
        Printk::Write(Printk::Level::LOG_INFO, " IRQ-driven input enabled on IRQ4 (vector 0x%02x)\n", (unsigned)(0x20 + 4));
    }

} // namespace Serial