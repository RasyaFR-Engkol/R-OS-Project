#pragma once

#include "ahci_regs.hpp"
#include <mm.hpp>

namespace AHCI {
    // Kita siapkan array untuk maks 4 controller
    #define MAX_AHCI_CONTROLLERS 4

    enum class DeviceType {
        NONE = 0,
        SATA = 1,
        SEMB = 2,
        PM = 3,
        SATAPI = 4
    };
    
    // Ini adalah "objek" driver kita
    struct AHCIDriver {
        volatile HBA_MEM* regs; // Pointer virtual ke register
        U8 bus, dev, func;
        bool initialized;
        U8 IntVector;

        // Per-port device type (NONE if no device present)
        DeviceType port_device[32];

        PageAlloc::DMAAlloc::DMABuffer *dma_cmd_list[32];

        // Alokasi DMA untuk FIS Receive Buffer (FB)
        PageAlloc::DMAAlloc::DMABuffer* dma_fis_buffers[32];

        PageAlloc::DMAAlloc::DMABuffer* dma_cmd_tables[32];
        
        // Ini adalah pointer VIRTUAL ke Command List
        // (biar C++ bisa nulis/baca)
        volatile HBA_CMD_HEADER* v_cmd_lists[32];
        volatile U8* v_cmd_tables[32]; // (Kita pakai U8* biar gampang di-offset)
    };
    
    // Ini daftarnya
    extern AHCIDriver g_ahci_controllers[MAX_AHCI_CONTROLLERS];
    extern int g_ahci_controller_count;

    // Fungsi inilah yang akan dipanggil oleh ScanBus
    VOID RegisterAHCIController(U8 Bus, U8 Device, U8 Function, U8 MSICapOffset);
    
    // Nanti, fungsi ini akan menginisialisasi SEMUA controller yg terdaftar
    VOID InitializeAllControllers();

    BOOL SendIdentify(AHCIDriver &Driver, VAL32 PortNum);

    // Read 'count' 512-byte sectors from 'lba' into a newly allocated DMA buffer.
    // Returns TRUE on success and sets *outBuf. Caller must FreeDMABuffer(*outBuf).
    BOOL ReadSectors(AHCIDriver &Driver, VAL32 PortNum, U64 lba, U32 count,
                     PageAlloc::DMAAlloc::DMABuffer **outBuf);

    // Write 'count' 512-byte sectors to 'lba' from an existing DMA buffer 'buf'.
    // Returns TRUE on success. Buffer must be at least count*512 bytes in size.
    BOOL WriteSectors(AHCIDriver &Driver, VAL32 PortNum, U64 lba, U32 count,
                      PageAlloc::DMAAlloc::DMABuffer *buf);

    VOID HandleInterrupt(VAL32 Controller_ID);
}