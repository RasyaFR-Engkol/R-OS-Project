/* Minimal TSS interface for ROS */
#pragma once

#include <rosval.h>

namespace TSS {
    // Initialize TSS and install into GDT. `rsp0_top` is the virtual address
    // of the top of the kernel stack (RSP value to use on ring transition).
    void Init(UPTR rsp0_top);

    // Update RSP0 at runtime (per-cpu use)
    void SetRsp0(UPTR rsp0_top);
}
