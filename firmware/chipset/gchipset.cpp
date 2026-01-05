#include "chipset.hpp"
#include <../kernel/driver/pci/pci.hpp>
#define PRINTK_MODULE_NAME "CHIPSET"
#include <logging.hpp>

namespace Firmware{
    ABI_C {
        namespace Chipset{
            ChipsetType g_DetectedChipset = CHIPSET_UNKNOWN;

            ChipsetType DetectMotherboardChipset(){
                    for (U8 dev = 0; dev < 32; dev++) {
                        U32 vendDev = PCI::ReadDword(0, dev, 0, 0x00);
                        U16 vendor = vendDev & 0xFFFF;
                        U16 device = (vendDev >> 16) & 0xFFFF;

                        if (vendor == PCI_VENDOR_INTEL) {
                            if (device == PCI_DEVICE_PIIX3) {
                                Printk::Write(Printk::Level::LOG_INFO, "[CHIPSET] Detected PIIX3 (Legacy Mode)\n");
                                return CHIPSET_PIIX3;
                            }
                            if (device == PCI_DEVICE_ICH9) {
                                Printk::Write(Printk::Level::LOG_INFO, "[CHIPSET] Detected ICH9 (Modern Mode)\n");
                                return CHIPSET_ICH9;
                            }
                        }
                    }

                    Printk::Write(Printk::Level::LOG_WARNING, "[CHIPSET] Unknown Chipset! Defaulting to PIIX3 logic.\n");
                return CHIPSET_PIIX3; // Fallback aman
            }
        }
    }
}