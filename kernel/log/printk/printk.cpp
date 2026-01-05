#include "printk.hpp"
#include <serial.hpp>
#include <string.hpp>
#include <port.hpp>
#include <rossys.hpp>
#include <spinlock/simple.hpp>
// Mirror logs to framebuffer console when available
#include "../fbcon/fbcon.hpp"
#include "framebuffer.hpp"
#include <filesystem/filesystem.hpp>
#include "../../filesys/vfs/vfs.hpp"
#include "rosval.h"
#include <../firmware/acpi/driver/timer/timer.hpp>

// Module-name is provided per-translation-unit via `ExportSymbol()` macro in
// the header (static inline helper). No global weak symbol is needed.

namespace Printk {
    using namespace String;
    using namespace Port;

    // Compile-time log level threshold. If PRINTK_CONFIG is defined by the
    // build/system headers it will be used; otherwise default to LOG_INFO
    // so that debug messages are hidden by default.
    #ifndef PRINTK_CONFIG
        #define PRINTK_CONFIG LOG_DOK
    #endif

    static char LogBuffer[4096];
    static size_t LogBufferIndex = 0;
    // Index of the earliest byte not yet flushed to serial
    static size_t LogBufferFlushIndex = 0;
    SPINLOCK_T PrintkLock;
    // Stores the last flushed message for use by emergency handler
    static CHAR8 LastFlushedMessage[sizeof(LogBuffer) + 1];

    // Optional VFS file handles for consoles (if /dev/ttyfb0 and /dev/ttyS0 are available)
    File* s_FrameConsoleFile = nullptr;
    File* s_SerialConsoleFile = nullptr;

    VOID Init(){
        Arch::Spinlock::SpinlockInit(&PrintkLock);
        Serial::Init();
        FBConsole::Init();
        // Try to open VFS console device nodes. If present, store handles
        // so we can write via VFS instead of direct console APIs.
        s_FrameConsoleFile = VFSManager::Open("/dev/ttyfb0", O_RDWR);
        s_SerialConsoleFile = VFSManager::Open("/dev/ttyS0", O_RDWR);
    }

    // Helper: write the given NUL-terminated string to both serial and
    // framebuffer consoles. Prefer writing via VFS file handles if they
    // are available; otherwise fall back to direct Serial/FBConsole APIs.
    static void WriteToConsolesViaVFSorFallback(const CHAR8* tmp) {
        if (!tmp) return;
        __MAYBE_UNUSED U32 len = (U32)Strlen(tmp);
        
        // Fallback to direct serial write if VFS handle is not available OR if VFS write fails
        if (s_SerialConsoleFile) {
            // Use direct write for now to debug VFS recursion/deadlock issues
            // VFSManager::Write(s_SerialConsoleFile, (U8*)tmp, len);
            Serial::Write(tmp);
        } else {
            Serial::Write(tmp);
        }

        if (s_FrameConsoleFile) {
            // Use direct write for now to debug VFS recursion/deadlock issues
            // VFSManager::Write(s_FrameConsoleFile, (U8*)tmp, len);
            if (FBConsole::IsReady()) {
                FBConsole::WriteString(tmp);
            }
        } else if (FBConsole::IsReady()) {
            FBConsole::WriteString(tmp);
        }
    }

    // Append string to log buffer; flush to serial on '\n' or '\0'.
    VOID WriteToLogBuffer(const CHAR8* s) {
        size_t len = Strlen(s);

        for (size_t i = 0; i < len; ++i) {
            CHAR8 ch = s[i];

            if (LogBufferIndex >= sizeof(LogBuffer)) {
                // If buffer is full, flush pending region to serial then wrap
                if (LogBufferFlushIndex < LogBufferIndex) {
                    const size_t CHUNK = 256;
                    CHAR8 tmp[CHUNK];
                    size_t pos = LogBufferFlushIndex;
                    while (pos < LogBufferIndex) {
                        size_t tocopy = ((LogBufferIndex - pos) < (CHUNK - 1)) ? (LogBufferIndex - pos) : (CHUNK - 1);
                        for (size_t k = 0; k < tocopy; ++k) tmp[k] = LogBuffer[pos + k];
                        tmp[tocopy] = '\0';
                        WriteToConsolesViaVFSorFallback(tmp);
                        pos += tocopy;
                    }
                }
                LogBufferIndex = 0;
                LogBufferFlushIndex = 0;
            }

            LogBuffer[LogBufferIndex++] = ch;

            // Flush to serial immediately on newline or NUL
            if (ch == '\n' || ch == '\0') {
                if (LogBufferFlushIndex < LogBufferIndex) {
                    const size_t CHUNK = 256;
                    CHAR8 tmp[CHUNK];
                    size_t pos = LogBufferFlushIndex;
                    while (pos < LogBufferIndex) {
                        size_t tocopy = ((LogBufferIndex - pos) < (CHUNK - 1)) ? (LogBufferIndex - pos) : (CHUNK - 1);
                        for (size_t k = 0; k < tocopy; ++k) tmp[k] = LogBuffer[pos + k];
                        tmp[tocopy] = '\0';
                        WriteToConsolesViaVFSorFallback(tmp);
                        pos += tocopy;
                    }
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
        const size_t CHUNK = 256;
        if (LogBufferFlushIndex == LogBufferIndex && !force) {
            WriteToConsolesViaVFSorFallback((const CHAR8*)"WARNING: PRINTK LOG BUFFER FULL!!\n");
            return;
        }

        if (LogBufferFlushIndex < LogBufferIndex) {
            // contiguous region
            CHAR8 tmp[CHUNK];
            size_t pos = LogBufferFlushIndex;
            size_t last_copied = 0;
            while (pos < LogBufferIndex) {
                size_t tocopy = ((LogBufferIndex - pos) < (CHUNK - 1)) ? (LogBufferIndex - pos) : (CHUNK - 1);
                for (size_t k = 0; k < tocopy; ++k) tmp[k] = LogBuffer[pos + k];
                tmp[tocopy] = '\0';
                WriteToConsolesViaVFSorFallback(tmp);
                for (size_t k = 0; k < tocopy && (last_copied + k) < sizeof(LastFlushedMessage) - 1; ++k) {
                    LastFlushedMessage[last_copied + k] = tmp[k];
                }
                last_copied += tocopy;
                pos += tocopy;
            }
            LastFlushedMessage[(last_copied < sizeof(LastFlushedMessage)) ? last_copied : (sizeof(LastFlushedMessage)-1)] = '\0';
            LogBufferFlushIndex = LogBufferIndex;
        } else if (force && LogBufferFlushIndex > LogBufferIndex) {
            // wrapped: flush [flushIndex, end) then [0, LogBufferIndex)
                    CHAR8 tmp[CHUNK];
            size_t last_copied = 0;
            // flush [flushIndex, end)
            size_t p = LogBufferFlushIndex;
            while (p < sizeof(LogBuffer)) {
                size_t tocopy = ((sizeof(LogBuffer) - p) < (CHUNK - 1)) ? (sizeof(LogBuffer) - p) : (CHUNK - 1);
                for (size_t k = 0; k < tocopy; ++k) tmp[k] = LogBuffer[p + k];
                tmp[tocopy] = '\0';
                WriteToConsolesViaVFSorFallback(tmp);
                for (size_t k = 0; k < tocopy && (last_copied + k) < sizeof(LastFlushedMessage) - 1; ++k) {
                    LastFlushedMessage[last_copied + k] = tmp[k];
                }
                last_copied += tocopy;
                p += tocopy;
            }
            // flush [0, LogBufferIndex)
            p = 0;
            while (p < LogBufferIndex) {
                size_t tocopy = ((LogBufferIndex - p) < (CHUNK - 1)) ? (LogBufferIndex - p) : (CHUNK - 1);
                for (size_t k = 0; k < tocopy; ++k) tmp[k] = LogBuffer[p + k];
                tmp[tocopy] = '\0';
                WriteToConsolesViaVFSorFallback(tmp);
                for (size_t k = 0; k < tocopy && (last_copied + k) < sizeof(LastFlushedMessage) - 1; ++k) {
                    LastFlushedMessage[last_copied + k] = tmp[k];
                }
                last_copied += tocopy;
                p += tocopy;
            }
            LastFlushedMessage[(last_copied < sizeof(LastFlushedMessage)) ? last_copied : (sizeof(LastFlushedMessage)-1)] = '\0';
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
    VOID DumpStackTrace() {
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

            CHAR8 Buffer[256];

            if (name && sym_addr != 0 && ret >= sym_addr) {
                UPTR offset = ret - sym_addr;
                String::SPrint(Buffer, sizeof(Buffer), "  #%02d: %s+0x%llx (%p)\n", i, name, (unsigned long long)offset, (void*)ret);
                VFSManager::Write(s_SerialConsoleFile, (U8*)Buffer, Strlen(Buffer));
                VFSManager::Write(s_FrameConsoleFile, (U8*)Buffer, Strlen(Buffer));
            } else {
                String::SPrint(Buffer, sizeof(Buffer), "  #%02d: %p <unknown>\n", i, (void*)ret);
                VFSManager::Write(s_SerialConsoleFile, (U8*)Buffer, Strlen(Buffer));
                VFSManager::Write(s_FrameConsoleFile, (U8*)Buffer, Strlen(Buffer));
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
        const CHAR8 *to_print = "[kernel-emergency-mode]_[system_fully_stopped] (end of)";
        VFSManager::Write(s_SerialConsoleFile, (U8*)to_print , (U32)Strlen(to_print));
        VFSManager::Write(s_FrameConsoleFile, (U8*)to_print, Strlen(to_print));
        // halt forever
        while (1) Arch::HaltCPU();
    }

    BOOL InternalWrite(Printk::Level level, const char *module_name, const char *fmt, VA_LIST Args) {
        // Filter out messages below the configured threshold to avoid
        // expensive formatting for debug logs when they're disabled.
        if (level > PRINTK_CONFIG) return TRUE;

        LOCKRFLAGS prev = Arch::SaveAndDisableInterrupts();
        Arch::Spinlock::SpinLockAcquire(&PrintkLock);

        U64 sec = 0, usec = 0;

        ACPI::Timer::GetTimeSinceBoot(&sec, &usec);

        CHAR8 tsBuffer[64];
        String::SPrint(tsBuffer, sizeof(tsBuffer), "[%5d.%06d] ", sec, usec);
        Printk::WriteToLogBuffer(tsBuffer);

        // Emit level prefix (restore levelling)
        const CHAR8* levelPrefix = "";
        switch (level) {
            case LOG_EMERG: levelPrefix = ANSI_RESET "(start) [kernel-emergency-mode]_[system_fully_stopped] "; break;
            case LOG_ALERT: levelPrefix = ANSI_FG_RED "[ALERT] " ANSI_RESET; break;
            case LOG_CRIT:  levelPrefix = ANSI_FG_RED "[CRIT] " ANSI_RESET; break;
            case LOG_ERR:   levelPrefix = ANSI_FG_RED "[ERR] " ANSI_RESET; break;
            case LOG_WARNING: levelPrefix = ANSI_FG_YELLOW "[WARN] " ANSI_RESET; break;
            case LOG_NOTICE:  levelPrefix = ANSI_FG_GREEN "[NOTICE] " ANSI_RESET; break;
            case LOG_INFO:    levelPrefix = ANSI_FG_GREEN "[INFO] " ANSI_RESET; break;
            case LOG_DEBUG:   levelPrefix = ANSI_FG_BRIGHT_CYAN "[DEBUG] " ANSI_RESET; break;
            case LOG_DINFO:  levelPrefix = ANSI_FG_CYAN "[DINFO] " ANSI_RESET; break;
            case LOG_DOK:    levelPrefix = ANSI_FG_CYAN "[DOK] " ANSI_RESET; break;
            case LOG_DERR:   levelPrefix = ANSI_FG_RED "[DERR] " ANSI_RESET; break;
            case LOG_DWARNING: levelPrefix = ANSI_FG_YELLOW "[DWARN] " ANSI_RESET; break;
            default: levelPrefix = "[LOG] "; break;
        }
        // write prefix to buffer (and it will flush to serial on '\n' if present)
        Printk::WriteToLogBuffer(levelPrefix);

        const CHAR8* mod = module_name ? module_name : "Kernel";
        WriteToLogBuffer("[");
        WriteToLogBuffer(mod);
        WriteToLogBuffer("] ");

        // Use VSPrint to format the rest of the message into a temporary buffer
        {
            CHAR8 temp[2048];
            // Args is a VA_LIST (typedef to va_list) and VSPrint expects va_list
            int written = VSPrint((char*)temp, sizeof(temp), fmt, Args);
            // Ensure null-terminated (VSPrint already does this when bufsize>0)
            temp[(written < (int)sizeof(temp)) ? written : (int)(sizeof(temp) - 1)] = '\0';
            Printk::WriteToLogBuffer(temp);
        }
        // After formatting finished, flush remaining content to serial
        FlushLogBufferToSerial(TRUE);

        Arch::Spinlock::SpinLockRelease(&PrintkLock);
        Arch::RestoreInterrupts(prev);

        /* 
         * UPDATE: 5 Desember 2025:
         * 
         * PANIC:
         * Pemindahan log PANIC ke fungsi tersendiri. Tujuannya biar
         * lebih terstruktur dan jelas. Juga menghindari potensi deadlock
         * atau isu lain yang mungkin muncul jika PANIC dipanggil dari dalam
         * Printk::Write() itu sendiri.
         * 
         * panggil handler Panic() nanti.
         * 
         * expectd log:
         * [Kernel Panic: System Fully Stopped] = "msg format disini"
         * .. DUMP CPU ..
         * .. DUMP STACK TRACE ..
         * .. DUMP LAST LOG MESSAGES ..
         * [end of Kernel Panic: System Fully Stopped] = "msg format disini"
         * 
         */

        return TRUE;
    }

    // NOTE: header previously declared a non-variadic Printk::Write(Level,const char*).
    // Prefer the variadic API `Write(Level, const char *fmt, ...)` for formatted output.
    
}