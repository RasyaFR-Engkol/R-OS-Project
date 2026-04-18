#pragma once

#include "ahci_regs.hpp"
#include <spinlock/mutex.hpp>
#include <mm.hpp>
#include <task.hpp>

namespace AHCI {
    // Kita siapkan array untuk maks 4 controller
    #define MAX_AHCI_CONTROLLERS 4
    #define MAX_AHCI_PORT 32

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
        // Interrupt routing bookkeeping
        bool using_msi;
        U8 msi_cap_offset; // 0 if none
        U8 legacy_irq;     // legacy IRQ number (0..15) if used, 0 if none

        // Saved routing/interrupt enable state for temporary reroute
        bool saved_valid;
        U32 saved_ghc;
        U32 saved_port_ie[32];
        // For MSI saved message address (low dword) and optionally high
        U32 saved_msi_addr_lo;
        U32 saved_msi_addr_hi;
        // For IOAPIC saved high dword of redirection entry
        U32 saved_ioapic_redt_high;

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

        // task preempted oleh interrupt AHCI
        Tasking::Task *WaitingTask[MAX_AHCI_PORT];

        Mutex PortLocks[MAX_AHCI_PORT];
    };

    struct AHCIPortInfo {
        U8 controller_index;
        U8 port_number;
        AHCIDriver AhciDRV;
    };
    
    // Ini daftarnya
    extern AHCIDriver g_ahci_controllers[MAX_AHCI_CONTROLLERS];
    extern int g_ahci_controller_count;
    extern Mutex DiskLock;

    // Fungsi inilah yang akan dipanggil oleh ScanBus
    VOID RegisterAHCIController(U8 Bus, U8 Device, U8 Function, U8 MSICapOffset);

    // Reroute the interrupt for controller `Index` to APIC ID `apicId`.
    // Returns TRUE on success.
    BOOL RerouteControllerInterrupt(U8 Index, U8 apicId);
    
    // Temporarily move interrupts for controller `Index` to `apicId`.
    // This will disable the device interrupt source(s), change routing,
    // then re-enable the device. The original routing and device IE bits
    // are saved so `RestoreControllerInterrupt` can reapply them.
    BOOL MoveControllerInterruptToCPU(U8 Index, U8 apicId);

    // Restore a previous temporary move performed by MoveControllerInterruptToCPU.
    BOOL RestoreControllerInterrupt(U8 Index);
    
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

    // Mengambil AHCI Controller berdasarkan index (0 .. g_ahci_controller_count-1)
    // Nilai kembali adalah salinan (copy) dari struct AHCIDriver.
    // Jika index tidak valid, 'regs' akan bernilai nullptr dan 'initialized' = false.
    AHCIDriver GetController(int index);

    // Mencari nomor port pertama yang aktif (device terdeteksi) pada sebuah controller.
    // Kriteria sfaat ini: port_device[port] == DeviceType::SATA.
    // Nilai kembali: index port (0..31) atau -1 jika tidak ada.
    VAL32 FindActivePortNum(const AHCIDriver &Driver);

    AHCIPortInfo GetPortInfo(int ConIndex = 0);
}