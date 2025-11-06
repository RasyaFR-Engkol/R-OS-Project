#define PRINTK_MODULE_NAME "PCI"
#include "pci.hpp"
#include <rosval.h>
#include <string.hpp>
#include <logging.hpp>
#include "../ahci/ahci.hpp"
#include "../xhci/xhci.hpp"

/* module name provided via PRINTK_MODULE_NAME */

namespace PCI{
    using namespace Printk;

    static U8 FindCapatibility(U8 Bus, U8 Device, U8 Function){
        U32 Reg4 = ReadDword(Bus, Device, Function, 0x04);
        U16 Status = (U16)((Reg4 >> 16) & 0xFFFF);

        if(!(Status & (1 << 4))){
            return 0; // Tidak ada capability list
        }

        U32 Reg34 = ReadDword(Bus, Device, Function, 0x34);
        U8 CapPTR = (U8)(Reg34 & 0xFF);
        
        while(CapPTR != 0x00){
            U32 CapHeader = ReadDword(Bus, Device, Function, CapPTR);
            U8 CapID = (U8)(CapHeader & 0xFF);
            U8 NextCapPTR = (U8)((CapHeader >> 8) & 0xFF);

            if(CapID == 0x05){
                Write(Level::LOG_NOTICE, "PCI %d:%d:%d - Found MSI Capability at offset 0x%02x\n",
                    (int)Bus, (int)Device, (int)Function, (int)CapPTR);
                return CapPTR;
            } else if(CapID == 0x11){
                Write(Level::LOG_NOTICE, "PCI %d:%d:%d - Found MSI-X Capability at offset 0x%02x\n",
                    (int)Bus, (int)Device, (int)Function, (int)CapPTR);
                // Here you can parse the MSI-X capability structure further if needed
            }

            CapPTR = NextCapPTR;
        }

        return 0;
    }

    void ScanBus(U8 bus){
        for(U8 device = 0; device < 32; device++){
            U16 VendorID = 0;
            BOOL IsMultiFunction = FALSE;

            for(U8 function = 0; function < 8; function++){
                U32 Reg0 = ReadDword(bus, device, function, 0x00);
                VendorID = (U16)(Reg0 & 0xFFFF);

                if (VendorID == 0xFFFF) {
                    continue; // Gak ada device di (B:D:F) ini, lanjut
                }

                U32 Reg8 = ReadDword(bus, device, function, 0x08);
                U8 ClassCode = (U8)((Reg8 >> 24) & 0xFF);
                U8 SubClass = (U8)((Reg8 >> 16) & 0xFF);
                U8 ProgIF = (U8)((Reg8 >> 8) & 0xFF);

                Write(Level::LOG_INFO, "PCI %d:%d:%d - Class:%x Subclass:%x\n",
                    (int)bus, (int)device, (int)function,
                    (int)ClassCode, (int)SubClass);

                U8 MSIOffset = FindCapatibility(bus, device, function);

                // Mencari AHCI disini
                if (ClassCode == 0x01 && SubClass == 0x06) {
                    Write(Level::LOG_INFO, "AHCI Controller Found in %d:%d:%d!\n",
                        (int)bus, (int)device, (int)function);
                    
                    AHCI::RegisterAHCIController(bus, device, function, MSIOffset);
                }
                
                // Mnecari xHCI
                if(ClassCode == 0x0C && SubClass == 0x03 && ProgIF == 0x30){
                    Write(Level::LOG_INFO, "USB XHCI Controller Found in %d:%d:%d!\n",
                        (int)bus, (int)device, (int)function);
                    xHCI::RegisterController(bus, device, function, MSIOffset);

                }

                if (function == 0) {
                    U32 regC = ReadDword(bus, device, function, 0x0C);
                    U8 headerType = (U8)((regC >> 16) & 0xFF);
                    if (headerType & 0x80) { // Cek 'multifunction bit'
                        IsMultiFunction = true;
                    }
                }
                
                if (!IsMultiFunction && function == 0) {
                    break; // Lanjut ke device berikutnya
                }
            }
        }
    }

    void ScanAllBuses(){
        Write(Level::LOG_INFO, " start scanning all buses\n");
        for(unsigned bus = 0; bus < 256; ++bus){
            ScanBus((U8)bus);
        }
    }

    VOID IntializePCIDrivers(){
        // Inisialisasi driver PCI di sini
        PCI::ScanAllBuses();

        // Inisialisasi modul PCI AHCI
        AHCI::InitializeAllControllers();

        // Inisialisasi modul PCI xHCI
        xHCI::InitializeAllControllers();
    }
}