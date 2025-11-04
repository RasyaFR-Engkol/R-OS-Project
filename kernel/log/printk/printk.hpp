#pragma once

#include <rosval.h>

namespace Printk {
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
    BOOL Write(Level, const char *fmt, ...);
}
