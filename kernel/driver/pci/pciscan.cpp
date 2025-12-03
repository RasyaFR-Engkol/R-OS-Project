#define PRINTK_MODULE_NAME "PCI"
#include "pci.hpp"
#include <rosval.h>
#include <string.hpp>
#include <logging.hpp>
#include "../ahci/ahci.hpp"
#include "../xhci/xhci.hpp"
#include "../nvme/nvme.hpp"
#include "../e1000/e1000.hpp"

/* module name provided via PRINTK_MODULE_NAME */

namespace PCI{
    using namespace Printk;

    U8 FindCapability(U8 Bus, U8 Device, U8 Function, U8 TargetCapID){
        U16 Status = ReadWord(Bus, Device, Function, 0x06); // Status Reg is at 0x06

        if(!(Status & (1 << 4))){
            return 0; // Bit 4 (Capabilities List) is 0
        }

        U8 CapPTR = ReadByte(Bus, Device, Function, 0x34); // Cap Ptr at 0x34
        
        // Loop limit biar gak infinite loop kalau hardware ngaco
        int loop = 0;
        while(CapPTR != 0x00 && loop < 48){
            U8 CapID = ReadByte(Bus, Device, Function, CapPTR);
            U8 NextCapPTR = ReadByte(Bus, Device, Function, CapPTR + 1);

            if(CapID == TargetCapID){
                // Ketemu!
                return CapPTR;
            }

            CapPTR = NextCapPTR;
            loop++;
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

                Write(Level::LOG_DEBUG, "PCI %d:%d:%d - Class:%x Subclass:%x\n",
                    (int)bus, (int)device, (int)function,
                    (int)ClassCode, (int)SubClass);

                // Mencari AHCI disini
                if (ClassCode == 0x01 && SubClass == 0x06) {
                    Write(Level::LOG_INFO, "AHCI Controller Found in %d:%d:%d!\n",
                        (int)bus, (int)device, (int)function);

                    U8 MSIOffset = FindCapability(bus, device, function, 0x05); // Cap ID 0x05 = MSI
                    
                    AHCI::RegisterAHCIController(bus, device, function, MSIOffset);
                }
                
                // Mnecari xHCI
                if(ClassCode == 0x0C && SubClass == 0x03 && ProgIF == 0x30){
                    Write(Level::LOG_INFO, "USB XHCI Controller Found in %d:%d:%d!\n",
                        (int)bus, (int)device, (int)function);
                    xHCI::RegisterController(bus, device, function, 0);

                }

                // Mencari NVMe
                if(ClassCode == 0x01 && SubClass == 0x08){
                    if(ProgIF == 0x02){
                        Printk::Write(Printk::Level::LOG_INFO, "NVMe Controller Found in %d:%d:%d!\n",
                            (int)bus, (int)device, (int)function);

                        NVMe::NVMeController::RegisterController(bus, device, function);
                    }
                }

                if(VendorID == 0x8086){
                    U32 devID = (Reg0 >> 16) & 0xFFFF;
                    if (devID == 0x100E || devID == 0x1000 || devID == 0x153A) {
                    Printk::Write(Printk::Level::LOG_INFO, "Intel E1000 NIC Found in %d:%d:%d!\n", 
                        (int)bus, (int)device, (int)function);
                        
                    // Include header e1000.hpp di atas file pciscan.cpp
                    Network::E1000::E1000Driver::RegisterDevice(bus, device, function);
                    }
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
        Write(Level::LOG_DEBUG, " start scanning all buses\n");
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