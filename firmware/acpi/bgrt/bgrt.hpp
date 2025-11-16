#pragma once

#include <rosval.h>

// Use our local lightweight ACPI header (no ACPICA dependency)
#include "../acpi.hpp"

namespace ACPI {
namespace BGRT {
    // Decode BGRT BMP and draw it to the framebuffer at the specified offsets.
    // Returns TRUE if a logo was drawn, FALSE otherwise.
    BOOL ShowLogoOnce();
}
}
