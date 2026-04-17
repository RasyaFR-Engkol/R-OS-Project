// This file has been modularized. See sibling files:
// - xhci_state.cpp: global driver state
// - xhci_pci.cpp: PCI registration and MSI hookup
// - xhci_init.cpp: controller initialization and setup
// - xhci_isr.cpp: ISR and event processing
// - xhci_cmd.cpp: command submission helpers
// - xhci_dump.cpp: diagnostic dumps
// - xhci_test.cpp: test helpers
// - xhci_endpoint.cpp: endpoint management
#include "rosval.h"
#define PRINTK_MODULE_NAME "XHCI"

#include <kernel_api.hpp>
#include "xhci.hpp"

// STRUCT KECIL KECILAN
KernelAPI *g_kernel_api = nullptr; // Pointer ke KernelAPI yang diisi saat module_init

ABI_C int module_init(KernelAPI *Api, VOID *PrivateData){
    if(!Api) return -1;
    g_kernel_api = Api; // Simpan pointer ke KernelAPI untuk digunakan di seluruh driver
    Api->Printk(Printk::LOG_INFO, "ROS-xHCI Driver initializing...\n");

    struct pci_data *busData = (struct pci_data*)PrivateData;

    xHCI::RegisterController(busData->bus, busData->device, busData->function, busData->msix_offset);

    xHCI::InitializeAllControllers();
    return 0;
}