#include "printk.hpp"
#include <serial.hpp>
#include <string.hpp>
#include <port.hpp>
#include <rossys.hpp>
#include <spinlock/simple.hpp>
// Mirror logs to framebuffer console when available
#include "../fbcon/fbcon.hpp"
#include "framebuffer.hpp"

// Module-name is provided per-translation-unit via `ExportSymbol()` macro in
// the header (static inline helper). No global weak symbol is needed.

namespace Printk {
    using namespace String;
    using namespace Port;

    static char LogBuffer[1024];
    static size_t LogBufferIndex = 0;
    // Index of the earliest byte not yet flushed to serial
    static size_t LogBufferFlushIndex = 0;
    SPINLOCK_T PrintkLock;
    // Stores the last flushed message for use by emergency handler
    static CHAR8 LastFlushedMessage[sizeof(LogBuffer) + 1];

    VOID Init(){
        Arch::Spinlock::SpinlockInit(&PrintkLock);
        Serial::Init();
        FBConsole::Init();
    }

    // Append string to log buffer; flush to serial on '\n' or '\0'.
    VOID WriteToLogBuffer(const CHAR8* s) {
        size_t len = Strlen(s);

        for (size_t i = 0; i < len; ++i) {
            CHAR8 ch = s[i];

            if (LogBufferIndex >= sizeof(LogBuffer)) {
                // If buffer is full, flush pending region to serial then wrap
                if (LogBufferFlushIndex < LogBufferIndex) {
                    size_t seglen = LogBufferIndex - LogBufferFlushIndex;
                    CHAR8 tmp[sizeof(LogBuffer) + 1];
                    for (size_t k = 0; k < seglen; ++k) tmp[k] = LogBuffer[LogBufferFlushIndex + k];
                    tmp[seglen] = '\0';
                    Serial::Write(tmp);
                    if (FBConsole::IsReady()) FBConsole::WriteString(tmp);
                }
                LogBufferIndex = 0;
                LogBufferFlushIndex = 0;
            }

            LogBuffer[LogBufferIndex++] = ch;

            // Flush to serial immediately on newline or NUL
            if (ch == '\n' || ch == '\0') {
                if (LogBufferFlushIndex < LogBufferIndex) {
                    size_t seglen = LogBufferIndex - LogBufferFlushIndex;
                    CHAR8 tmp[sizeof(LogBuffer) + 1];
                    for (size_t k = 0; k < seglen; ++k) tmp[k] = LogBuffer[LogBufferFlushIndex + k];
                    tmp[seglen] = '\0';
                    Serial::Write(tmp);
                    if (FBConsole::IsReady()) FBConsole::WriteString(tmp);
                    LogBufferFlushIndex = LogBufferIndex;
                }
            }
        }

        // Null-terminate current buffer position
        if (LogBufferIndex < sizeof(LogBuffer)) LogBuffer[LogBufferIndex] = '\0';
    }

    // Helper to write a single character into the log buffer
    static VOID WriteToLogBufferChar(CHAR8 c) {
        CHAR8 tmp[2];
        tmp[0] = c;
        tmp[1] = '\0';
        WriteToLogBuffer(tmp);
    }

    // Flush any pending data in log buffer to serial. If force==TRUE,
    // flush all pending bytes regardless of newline. Handles wrap-around.
    static VOID FlushLogBufferToSerial(BOOL force) {
        if (LogBufferFlushIndex == LogBufferIndex && !force) return;

        if (LogBufferFlushIndex < LogBufferIndex) {
            // contiguous region
            size_t seglen = LogBufferIndex - LogBufferFlushIndex;
            CHAR8 tmp[sizeof(LogBuffer) + 1];
            for (size_t k = 0; k < seglen; ++k) tmp[k] = LogBuffer[LogBufferFlushIndex + k];
            tmp[seglen] = '\0';
            Serial::Write(tmp);
            if (FBConsole::IsReady()) FBConsole::WriteString(tmp);
            // store last flushed message copy for emergency reporting
            for (size_t k = 0; k <= seglen; ++k) LastFlushedMessage[k] = tmp[k];
            LogBufferFlushIndex = LogBufferIndex;
        } else if (force && LogBufferFlushIndex > LogBufferIndex) {
            // wrapped: flush [flushIndex, end) then [0, LogBufferIndex)
            size_t seglen1 = sizeof(LogBuffer) - LogBufferFlushIndex;
            CHAR8 tmp[sizeof(LogBuffer) + 1];
            size_t pos = 0;
            for (size_t k = 0; k < seglen1; ++k) tmp[pos++] = LogBuffer[LogBufferFlushIndex + k];
            for (size_t k = 0; k < LogBufferIndex; ++k) tmp[pos++] = LogBuffer[k];
            tmp[pos] = '\0';
                Serial::Write(tmp);
                if (FBConsole::IsReady()) FBConsole::WriteString(tmp);
                // store last flushed message copy for emergency reporting
                for (size_t k = 0; k <= pos; ++k) LastFlushedMessage[k] = tmp[k];
            LogBufferFlushIndex = LogBufferIndex;
        }
    }

    // Weak symbol lookup hook. If a real symbol table is linked in it should
    // provide LookupKernelSymbol() which returns the symbol name and fills
    // *sym_addr with the symbol start address. Defaults to a stub that returns
    // NULL (no symbol available).
    extern "C" const CHAR8* LookupKernelSymbol(UPTR addr, UPTR* sym_addr) __attribute__((weak));

    // Dump a stack trace by walking frame pointers. This prints return
    // addresses and, when available, the symbol + offset using
    // LookupKernelSymbol(). The walker is defensive: it checks for NULL/low
    // pointers and limits depth to avoid runaway loops.
    static VOID DumpStackTrace() {
        Serial::Write("[PANIC] Stack trace:\n");
        UPTR *rbp = nullptr;
        asm volatile ("mov %%rbp, %0" : "=r"(rbp));

        const int MAX_FRAMES = 64;
        for (int i = 0; i < MAX_FRAMES; ++i) {
            if (!rbp) break;

            // Basic sanity checks: reject obviously invalid rbp values
            UPTR rbpv = (UPTR)rbp;
            if (rbpv < 0x1000ULL) break; // too low / NULL

            // Try to read saved return address at [rbp + 1]
            UPTR ret = 0;
            // avoid crashing on malformed rbp by checking the pointer we deref
            // Note: in a freestanding kernel we can't easily probe user memory
            // safely; this basic guard is pragmatic for QEMU/testing.
            UPTR *saved_rbp = nullptr;
            saved_rbp = (UPTR*)(*rbp);
            // If saved_rbp looks invalid, still try to read return address but
            // break afterwards.
            ret = (UPTR) *(rbp + 1);

            // Try to resolve symbol name (if symbol lookup provided)
            UPTR sym_addr = 0;
            const CHAR8* name = nullptr;
            if (LookupKernelSymbol) name = LookupKernelSymbol(ret, &sym_addr);

            if (name && sym_addr != 0 && ret >= sym_addr) {
                UPTR offset = ret - sym_addr;
                Serial::Printf("  #%02d: %s+0x%llx (%p)\n", i, name, (unsigned long long)offset, (void*)ret);
            } else {
                // Symbol not found: print address and an explicit <unknown> marker
                Serial::Printf("  #%02d: %p <unknown>\n", i, (void*)ret);
            }

            // Stop if saved_rbp is null or doesn't move forward in the stack
            if (!saved_rbp) break;
            UPTR saved_v = (UPTR)saved_rbp;
            if (saved_v <= rbpv) break;
            rbp = saved_rbp;
        }
    }

    // Handle EMERG: disable interrupts, flush logs, dump stack and halt
    static VOID HandleEmergAndHalt(const char *msg) {
        // ensure prefix and pending data flushed
        FlushLogBufferToSerial(TRUE);
        // disable interrupts
        Arch::Cli();
        // dump stack trace to serial
        DumpStackTrace();
        // final message
        // Prefer the explicit last-flushed message if provided, otherwise fall back
        const CHAR8 *to_print = msg ? msg : LastFlushedMessage;
        Serial::Printf("[kernel-emergency-mode]_[system_fully_stopped] %s (end of)\n", to_print);
        // halt forever
        while (1) Arch::HaltCPU();
    }

    BOOL InternalWrite(Printk::Level level, const char *module_name, const char *fmt, VA_LIST Args) {
        // Emit level prefix (restore levelling)
        const CHAR8* levelPrefix = "";
        switch (level) {
            case LOG_EMERG: levelPrefix = "(start) [kernel-emergency-mode]_[system_fully_stopped] "; break;
            case LOG_ALERT: levelPrefix = "[ALERT] "; break;
            case LOG_CRIT:  levelPrefix = "[CRIT] "; break;
            case LOG_ERR:   levelPrefix = "[ERR] "; break;
            case LOG_WARNING: levelPrefix = "[WARN] "; break;
            case LOG_NOTICE:  levelPrefix = "[NOTICE] "; break;
            case LOG_INFO:    levelPrefix = "[INFO] "; break;
            case LOG_DEBUG:   levelPrefix = "[DEBUG] "; break;
            default: levelPrefix = "[LOG] "; break;
        }
        // write prefix to buffer (and it will flush to serial on '\n' if present)
        Printk::WriteToLogBuffer(levelPrefix);

        const CHAR8* mod = module_name ? module_name : "Kernel";
        WriteToLogBuffer("[");
        WriteToLogBuffer(mod);
        WriteToLogBuffer("] ");

        // ketika masih ada karakter *fmt, kita akan masuk ke case
        // *fmt, lalu kita mulai iterasi 1 1 karakternya untuk di printf
        while(*fmt) {
            // Jika FMT ada persenan, make itu adalah format, make
            // kita harus masuk ke iterasi pemrosesan
            if(*fmt == '%') {
                // Skip persenan, naikan fmt
                fmt++;

                BOOL ZeroPadding = FALSE;
                BOOL LeftJustify = FALSE;
                // Parse flags: '-', '0' (we support left-justify and zero-pad)
                if (*fmt == '-') {
                    LeftJustify = TRUE; fmt++;
                }
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

                // Parse precision (optional) and length modifier: l, ll, z
                BOOL HasPrecision = FALSE;
                VAL32 Precision = 0;
                if (*fmt == '.') {
                    fmt++;
                    HasPrecision = TRUE;
                    while (*fmt >= '0' && *fmt <= '9') {
                        Precision = Precision * 10 + (*fmt - '0');
                        fmt++;
                    }
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
                        Printk::WriteToLogBuffer("%");
                        break;
                    }
                    case 's': {
                        str = va_arg(Args, const CHAR8*);
                        if (!str) str = "(null)";
                        unsigned long long slen = Strlen(str);
                        if (HasPrecision && Precision < (VAL32)slen) slen = Precision;
                        int pad = (Width > (int)slen) ? (Width - (int)slen) : 0;
                        if (!LeftJustify) {
                            for (int i = 0; i < pad; ++i) Printk::WriteToLogBuffer(" ");
                            for (unsigned long long k = 0; k < slen; ++k) Printk::WriteToLogBufferChar(str[k]);
                        } else {
                            for (unsigned long long k = 0; k < slen; ++k) Printk::WriteToLogBufferChar(str[k]);
                            for (int i = 0; i < pad; ++i) Printk::WriteToLogBuffer(" ");
                        }
                        break;
                    }
                    case 'c': {
                        int ch = va_arg(Args, int);
                        CHAR8 tmpc[2] = { (CHAR8)ch, '\0' };
                        Printk::WriteToLogBuffer(tmpc);
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
                        int num_digits = (int)len;
                        int total_len = num_digits + (neg ? 1 : 0);
                        // If precision specified for integers, it defines minimum digits
                        int min_digits = 0;
                        if (HasPrecision) min_digits = (int)Precision;
                        int zero_pad = 0;
                        if (min_digits > num_digits) zero_pad = min_digits - num_digits;
                        // If no precision, ZeroPadding flag may request zero pad to width
                        if (!HasPrecision && ZeroPadding && !LeftJustify && Width > total_len) zero_pad = Width - total_len;

                        if (LeftJustify) {
                            if (neg) Printk::WriteToLogBufferChar('-');
                            for (int z = 0; z < zero_pad; ++z) Printk::WriteToLogBufferChar('0');
                            Printk::WriteToLogBuffer(out);
                            for (int i = total_len + zero_pad; i < Width; ++i) Printk::WriteToLogBufferChar(' ');
                        } else {
                            // pad spaces before sign if no zero padding
                            if (zero_pad == 0) {
                                for (int i = total_len; i < Width; ++i) Printk::WriteToLogBufferChar(' ');
                                if (neg) Printk::WriteToLogBufferChar('-');
                            } else {
                                if (neg) Printk::WriteToLogBufferChar('-');
                                for (int z = 0; z < zero_pad; ++z) Printk::WriteToLogBufferChar('0');
                            }
                            Printk::WriteToLogBuffer(out);
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
                        int num_digits = (int)len;
                        int min_digits = HasPrecision ? (int)Precision : 0;
                        int zero_pad = (min_digits > num_digits) ? (min_digits - num_digits) : 0;
                        if (!HasPrecision && ZeroPadding && !LeftJustify && Width > num_digits) zero_pad = Width - num_digits;
                        if (LeftJustify) {
                            for (int z = 0; z < zero_pad; ++z) Printk::WriteToLogBufferChar('0');
                            Printk::WriteToLogBuffer(Buf);
                            for (int i = num_digits + zero_pad; i < Width; ++i) Printk::WriteToLogBufferChar(' ');
                        } else {
                            for (int i = num_digits + zero_pad; i < Width; ++i) Printk::WriteToLogBufferChar(' ');
                            for (int z = 0; z < zero_pad; ++z) Printk::WriteToLogBufferChar('0');
                            Printk::WriteToLogBuffer(Buf);
                        }
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
                        int num_digits = (int)len;
                        int min_digits = HasPrecision ? (int)Precision : 0;
                        int zero_pad = (min_digits > num_digits) ? (min_digits - num_digits) : 0;
                        if (!HasPrecision && ZeroPadding && !LeftJustify && Width > num_digits) zero_pad = Width - num_digits;
                        if (LeftJustify) {
                            for (int z = 0; z < zero_pad; ++z) Printk::WriteToLogBufferChar('0');
                            Printk::WriteToLogBuffer(Buf);
                            for (int i = num_digits + zero_pad; i < Width; ++i) Printk::WriteToLogBufferChar(' ');
                        } else {
                            for (int i = num_digits + zero_pad; i < Width; ++i) Printk::WriteToLogBufferChar(' ');
                            for (int z = 0; z < zero_pad; ++z) Printk::WriteToLogBufferChar('0');
                            Printk::WriteToLogBuffer(Buf);
                        }
                        break;
                    }
                    case 'f': {
#if defined(__SSE__) || defined(__AVX__) || defined(__SSE2__)
                        /* Minimal floating-point formatting: support precision (default 6), sign,
                           integer part and fractional part with rounding. Uses only existing
                           Itoa/Utoa helpers and integer arithmetic for the fractional digits. */
                        double fval = va_arg(Args, double);
                        bool neg = false;
                        if (fval < 0.0) { neg = true; fval = -fval; }

                        int prec = HasPrecision ? (int)Precision : 6; // default precision 6
                        if (prec < 0) prec = 6;

                        // Extract integer part
                        long long intpart = (long long)fval;
                        double frac = fval - (double)intpart;

                        // Prepare multiplier = 10^prec
                        unsigned long long mul = 1ULL;
                        for (int i = 0; i < prec; ++i) mul *= 10ULL;

                        // Compute fractional digits as integer with rounding
                        unsigned long long frac_digits = 0ULL;
                        {
                            double tmp = frac * (double)mul;
                            unsigned long long rounded = (unsigned long long)(tmp + 0.5);
                            if (rounded >= mul) {
                                intpart += 1;
                                rounded -= mul;
                            }
                            frac_digits = rounded;
                        }

                        // Convert integer part
                        CHAR8 intbuf[48];
                        Utoa((unsigned long long)intpart, intbuf, 10);

                        // Build fractional string with leading zeros if needed
                        CHAR8 fracbuf[64];
                        if (prec > 0) {
                            for (int i = prec - 1; i >= 0; --i) {
                                fracbuf[i] = (CHAR8)('0' + (frac_digits % 10ULL));
                                frac_digits /= 10ULL;
                            }
                            fracbuf[prec] = '\0';
                        } else {
                            fracbuf[0] = '\0';
                        }

                        /* Build the whole representation into a temporary buffer then apply padding */
                        CHAR8 whole[128];
                        size_t pos = 0;
                        if (neg) whole[pos++] = '-';
                        for (size_t i = 0; intbuf[i]; ++i) whole[pos++] = intbuf[i];
                        if (prec > 0) {
                            whole[pos++] = '.';
                            for (int i = 0; i < prec; ++i) whole[pos++] = fracbuf[i];
                        }
                        whole[pos] = '\0';

                        unsigned long long wlen = Strlen(whole);
                        int pad = (Width > (int)wlen) ? (Width - (int)wlen) : 0;
                        if (!LeftJustify) {
                            if (ZeroPadding && !HasPrecision) {
                                for (int i = 0; i < pad; ++i) Printk::WriteToLogBufferChar('0');
                            } else {
                                for (int i = 0; i < pad; ++i) Printk::WriteToLogBufferChar(' ');
                            }
                        }
                        Printk::WriteToLogBuffer(whole);
                        if (LeftJustify) {
                            for (int i = 0; i < pad; ++i) Printk::WriteToLogBufferChar(' ');
                        }
#else
                        /* Floating-point varargs are not supported under the current
                           compiler flags (SSE disabled). Print a fallback marker while
                           keeping width/left-justify behavior. */
                        CHAR8 whole_fallback[] = "<float>";
                        unsigned long long wlen = Strlen(whole_fallback);
                        int pad = (Width > (int)wlen) ? (Width - (int)wlen) : 0;
                        if (!LeftJustify) {
                            for (int i = 0; i < pad; ++i) Printk::WriteToLogBufferChar(' ');
                        }
                        Printk::WriteToLogBuffer(whole_fallback);
                        if (LeftJustify) {
                            for (int i = 0; i < pad; ++i) Printk::WriteToLogBufferChar(' ');
                        }
#endif
                        break;
                    }
                    case 'p': {
                        void* ptr = va_arg(Args, void*);
                        unsigned long long pv = (unsigned long long)ptr;
                        Utoa(pv, Buf, 16);
                        Printk::WriteToLogBuffer("0x");
                        Printk::WriteToLogBuffer(Buf);
                        break;
                    }
                    default: {
                        // Unknown specifier: print it literally
                        Printk::WriteToLogBufferChar('%');
                        if (*fmt) Printk::WriteToLogBufferChar(*fmt);
                        break;
                    }
                }
            } else {
                Printk::WriteToLogBufferChar(*fmt);
            }
            fmt++;
        }
        // After formatting finished, flush remaining content to serial
        FlushLogBufferToSerial(TRUE);

        if (level == LOG_EMERG) {
            // This function will not return (halts)
            // After flushing, LastFlushedMessage contains the formatted output;
            // pass that to the emergency handler so the final message is shown.
            HandleEmergAndHalt(LastFlushedMessage);
        }

        return TRUE;
    }

    // NOTE: header previously declared a non-variadic Printk::Write(Level,const char*).
    // Prefer the variadic API `Write(Level, const char *fmt, ...)` for formatted output.
    
}