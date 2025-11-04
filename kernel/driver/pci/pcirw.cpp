#include "pci.hpp"
#include <rossys.hpp>
#include <rosval.h>
#include <logging.hpp>

namespace PCI{
    using namespace Printk;

    U32 ReadDword(U8 Bus, U8 Device, U8 Function, U8 Offset){
        U32 LBus = (U32)Bus;
        U32 LDevice = (U32)Device;
        U32 LFunc = (U32)Function;
        U32 Address = (U32)((LBus << 16) | (LDevice << 11) | (LFunc << 8) | (Offset & 0xFC) | 0x80000000);

        // Tulis alamat ke port config address
        Port::Outl(0xCF8, Address);

        // Baca hasilnya
        return Port::Inl(0xCFC);
    }

        void WriteDword(U8 Bus, U8 Device, U8 Function, U8 Offset, U32 Value){
            U32 LBus = (U32)Bus;
            U32 LDevice = (U32)Device;
            U32 LFunc = (U32)Function;
            U32 Address = (U32)((LBus << 16) | (LDevice << 11) | (LFunc << 8) | (Offset & 0xFC) | 0x80000000);

            // Write address to config address port
            Port::Outl(0xCF8, Address);
            // Write the value to config data port
            Port::Outl(0xCFC, Value);
        }
}