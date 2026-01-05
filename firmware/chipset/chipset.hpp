#pragma once

#define PCI_VENDOR_INTEL    0x8086
#define PCI_DEVICE_PIIX3    0x7000
#define PCI_DEVICE_ICH9     0x2918

#include <rosval.h>

/*
 * Chipset.hpp:
 * Fungsionalitas:
 * Digunakan untuk mencari PIN Chipset Type. Bisa PIIX3, ICH9, dll.
 * 
 * Untuk menentukan apakah kita harus menggunakan mode legacy atau mode modern
 * dalam mengakses perangkat-perangkat tertentu seperti PIC, RTC, dsb. 
 */

namespace Firmware{
    ABI_C {
        namespace Chipset{
            enum ChipsetType{
                CHIPSET_UNKNOWN = 0,
                CHIPSET_PIIX3,
                CHIPSET_ICH9
            };

            extern ChipsetType g_DetectedChipset;

            ChipsetType DetectMotherboardChipset();
        }
    }
}