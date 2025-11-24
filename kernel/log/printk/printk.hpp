#pragma once

#include "serial.hpp"
#include "spinlock/simple.hpp"
#include <rosval.h>
#include "../../filesys/filesystem.hpp"
/* Per-translation-unit module name provider.
   Users can place `ExportSymbol("NAME")` in a module (after includes)
   to override the default module name for `Printk::Write()` calls in that TU.

   Implementation: define a static inline helper `__printk_module_name_impl()`
   that returns the module name; the header provides a default returning
   nullptr. `ExportSymbol(name)` defines the same helper in the TU which
   overrides the default (static inline has internal linkage per TU).
*/
/* Allow modules to set `PRINTK_MODULE_NAME` before including this header.
    If defined, use that as the module name; otherwise the per-TU
    `ExportSymbol` macro is supported below (but prefer defining
    `PRINTK_MODULE_NAME` before including for clarity).
*/
#ifdef PRINTK_MODULE_NAME
static inline const char* __printk_module_name_impl(void) { return PRINTK_MODULE_NAME; }
#else
/* Default module name when none is provided via PRINTK_MODULE_NAME or
    ExportSymbol: use a neutral label "Module" so logging always shows a
    readable module name instead of NULL. */
static inline const char* __printk_module_name_impl(void) { return "Module"; }
#endif

/* Backwards-compatible ExportSymbol macro (still available, but it must be
    used at global scope after this header is included). Prefer defining
    `PRINTK_MODULE_NAME` before including instead of using ExportSymbol.
*/
#define ExportSymbol(name) \
     static inline const char* __printk_module_name_impl(void) { return name; }

namespace Printk {

    extern File* s_SerialConsoleFile;
    extern File* s_FrameConsoleFile;

    typedef enum {
        LOG_EMERG = 1,
        LOG_ALERT = 2,
        LOG_CRIT  = 3,
        LOG_ERR   = 4,
        LOG_WARNING = 5,
        LOG_NOTICE  = 6,
        LOG_INFO    = 7,
        LOG_DEBUG   = 8
    }Level;

    VOID RateLimitCheck();
    VOID RateLimitReset();
    BOOL IsRateLimited();
    /* InternalWrite now accepts a VA_LIST so callers can forward va_list
       directly. Use the variadic `Write` helper to build the va_list. */
    BOOL InternalWrite(Level level, const char *module_name, const char *fmt, VA_LIST args);
    extern SPINLOCK_T PrintkLock;
    static inline BOOL Write(Level level, const char *fmt, ...) {
        VA_LIST args;
        VA_STRT(args, fmt);
        BOOL result = InternalWrite(level, __printk_module_name_impl(), fmt, args);
        VA_END(args);
        return result;
    }

    VOID Init();
}
