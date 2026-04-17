#pragma once

namespace xHCI {
    // ISR entry for controller 0 (used when enabling MSI)
    void xHCI_InterruptHandler_C0(void *context);
}
