// msi.hpp (atau msixmsi.hpp)
#pragma once
#include <rosval.h>
#include <interrupt.hpp> //(Kita butuh IDT::)
#include "../../pci.hpp" // (Kita butuh PCI::)

namespace MSI {

    // Enable MSI and register handler. Handler receives a void* context.
    U8 EnableMSI(U8 bus, U8 dev, U8 func, U8 msi_cap_offset, void (*handler)(void *context));
    
    // Set MSI message address to target APIC ID (xAPIC dest in bits 19:12).
    // Returns TRUE on success.
    BOOL SetMSIDestination(U8 bus, U8 dev, U8 func, U8 msi_cap_offset, U8 apic_id);
    
    // TODO: Tambahkan EnableMSIX nanti
}