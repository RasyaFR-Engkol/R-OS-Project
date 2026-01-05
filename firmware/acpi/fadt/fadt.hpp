#pragma once

#include "../acpi.hpp"
#include "rosval.h"

namespace ACPI {
    // Public declaration for FADT parser (implemented in fadt.cpp)
    VOID ParseFADT();
    VOID Enable();
}
