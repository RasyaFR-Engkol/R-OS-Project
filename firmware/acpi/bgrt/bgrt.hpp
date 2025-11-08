#pragma once

#include <rosval.h>

extern "C" {
    #include "../../acpica/source/include/acpi.h"
}

namespace ACPI {
namespace BGRT {
    // Decode BGRT BMP and draw it to the framebuffer at the specified offsets.
    // Returns TRUE if a logo was drawn, FALSE otherwise.
    BOOL ShowLogoOnce();
}
}
