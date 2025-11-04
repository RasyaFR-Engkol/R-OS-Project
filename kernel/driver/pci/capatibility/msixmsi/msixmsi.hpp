// msi.hpp (atau msixmsi.hpp)
#pragma once
#include <rosval.h>
#include <interrupt.hpp> //(Kita butuh IDT::)
#include "../../pci.hpp" // (Kita butuh PCI::)

namespace MSI {

    U8 EnableMSI(U8 bus, U8 dev, U8 func, U8 msi_cap_offset, void (*handler)(void));
    
    // TODO: Tambahkan EnableMSIX nanti
}