#pragma once

#include "mm.hpp"

namespace Paging {
    // Create ring-3 code/data descriptors in the GDT. Safe to call after
    // the kernel relocates/loads the GDT (i.e. GDT is accessible in HHDM).
    void AddUserGDTEntries();
}
