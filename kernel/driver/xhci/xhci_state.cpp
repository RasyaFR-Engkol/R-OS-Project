#include <rosval.h>
#include "xhci.hpp"

namespace xHCI {
    xHCIDriver g_xhci_controllers[XHCI_MAX_CONTROLLERS];
    int g_xhci_controller_count = 0;
}
