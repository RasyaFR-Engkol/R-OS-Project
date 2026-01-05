#include <rosval.h>
#define PRINTK_MODULE_NAME "StorageMgr"
#include "devicemanager.hpp"
#include "../driver/ahci/ahci_internal.hpp"
#include "../driver/nvme/nvme.hpp"

namespace DeviceManager{
    namespace StorageManager{
        VOID SyncAllStorageDevices(){
            // AHCI SYNC
            for(INTN i = 0; i < AHCI::g_ahci_controller_count ; i++){
                if(AHCI::g_ahci_controllers[i].initialized){
                    AHCI::AHCIDriver &DRV = AHCI::g_ahci_controllers[i];
                    for(INTN portnum = 0; portnum < 32; portnum++){
                        if(DRV.port_device[portnum] == AHCI::DeviceType::SATA){
                            BOOL ok = AHCI::FlushCache(DRV, (VAL32)portnum);
                            if(!ok){
                                Printk::Write(Printk::Level::LOG_ERR, " StorageMgr: AHCI Controller %d Port %d FlushCache failed\n",
                                    (int)i, (int)portnum);
                            } else {
                                Printk::Write(Printk::Level::LOG_DEBUG, " StorageMgr: AHCI Controller %d Port %d FlushCache succeeded\n",
                                    (int)i, (int)portnum);
                            }
                        }
                    }
                }
            }

            // NVMe SYNC
            if(NVMe::g_NVMeController){
                BOOL ok = NVMe::g_NVMeController->FlushCache();
                if(!ok){
                    Printk::Write(Printk::Level::LOG_ERR, " StorageMgr: NVMe Controller FlushCaches failed\n");
                } else {
                    Printk::Write(Printk::Level::LOG_DEBUG, " StorageMgr: NVMe Controller FlushCaches succeeded\n");
                }
            }
        }
    }
}