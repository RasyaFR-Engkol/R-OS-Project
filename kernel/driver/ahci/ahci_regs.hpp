#pragma once
#include <rosval.h> // (Ini untuk U8, U16, U32, U64)

// Penting: Semua struct ini HARUS
// 1. __attribute__((packed)) -> Biar C++ gak nambahin padding
// 2. Dialokasikan di memori fisik (pakai DMAAlloc kamu)

// ---------------------------------------------------
// Level 5: Formulir Perintah (Host-to-Device)
// ---------------------------------------------------
typedef struct __attribute__((packed)) {
    U8  fis_type;       // Tipe FIS (0x27 = H2D Register)
    U8  pmport_c;       // Port multiplier & C-bit
    U8  command;        // Perintah ATA (0x25=READ_DMA_EXT, 0x35=WRITE_DMA_EXT)
    U8  features_low;   // Fitur (LBA 7-0)

    U8  lba0;           // LBA 7-0
    U8  lba1;           // LBA 15-8
    U8  lba2;           // LBA 23-16
    U8  device;         // Device (LBA mode=0x40)

    U8  lba3;           // LBA 31-24
    U8  lba4;           // LBA 39-32
    U8  lba5;           // LBA 47-40
    U8  features_high;  // Fitur (LBA 15-8)

    U16 count;          // Jumlah sektor (count 0 = 65536)
    U8  icc;            // Isochronous Command Completion
    U8  control;        // Control
    U8  rsv1[4];        // Reserved
} FIS_REG_H2D;

// ---------------------------------------------------
// Level 4: Alamat DMA (Physical Region Descriptor)
// ---------------------------------------------------
typedef struct __attribute__((packed)) {
    U64 dba;            // Alamat Fisik Buffer (DBA)
    U32 rsv0;           // Reserved
    U32 dbc;            // Byte Count-1 in bits [21:0]; bit 31 = IOC (Interrupt on Completion)
} HBA_PRDT_ENTRY;

// ---------------------------------------------------
// Level 3: Dokumen Perintah (Command Table)
// ---------------------------------------------------
typedef struct __attribute__((packed)) {
    // 1. Formulir Perintah (CFIS)
    //    Perintah yg akan dikirim ke device (max 64 bytes)
    U8 cfis[64];

    // 2. Perintah ATAPI (kosongkan jika bukan ATAPI)
    U8 acmd[16];

    // 3. Reserved
    U8 rsv[48];

    // 4. Daftar Alamat DMA (PRDT)
    //    Ini adalah array. Ukuran (PRDTL) di-set di Command Header.
    //    Kita definisikan 1 aja di sini sebagai placeholder.
    HBA_PRDT_ENTRY prdt_entry[1];
} HBA_CMD_TBL;

// ---------------------------------------------------
// Level 2: Map Gantung (Command Header)
// ---------------------------------------------------
typedef struct __attribute__((packed)) {
    // Word 0
    U8  cfl:5;          // Command FIS Length (dword)
    U8  a:1;            // ATAPI
    U8  w:1;            // Write (H2D)
    U8  p:1;            // Prefetchable
    U8  r:1;            // Reset
    U8  b:1;            // BIST
    U8  c:1;            // Clear Busy
    U8  rsv0:1;
    U8  pmp:4;          // Port Multiplier Port
    U16 prdtl;          // Physical Region Descriptor Table Length (jumlah entry)

    // Word 1
    volatile U32 prdbc;// Physical Region Descriptor Byte Count (total bytes)

    // Word 2 & 3
    U64 ctba;           // Command Table Base Address (Alamat Fisik HBA_CMD_TBL)

    // Word 4-7
    U32 rsv1[4];        // Reserved
} HBA_CMD_HEADER;

// ---------------------------------------------------
// Level 1: Laci (Port Registers)
// ---------------------------------------------------
typedef struct __attribute__((packed)) {
    U64 clb;            // Command List Base Address (Alamat Fisik HBA_CMD_HEADER[32])
    U64 fb;             // FIS Base Address (Alamat Fisik Received FIS)
    U32 is;             // Interrupt Status
    U32 ie;             // Interrupt Enable
    U32 cmd;            // Command and Status
    U32 rsv0;           // Reserved
    U32 tfd;            // Task File Data
    U32 sig;            // Signature
    U32 ssts;           // SATA Status (SCR0)
    U32 sctl;           // SATA Control (SCR2)
    U32 serr;           // SATA Error (SCR1)
    U32 sact;           // SATA Active (SCR3)
    U32 ci;             // Command Issue
    U32 sntf;           // SATA Notification (SCR4)
    U32 fbs;            // FIS-based Switching Control
    U32 rsv1[11];
    U32 vendor[4];      // Vendor specific
} volatile HBA_PORT;

// ---------------------------------------------------
// Level 0: Lemari Arsip (HBA Memory)
// ---------------------------------------------------
typedef struct __attribute__((packed)) {
    // 0x00 - 0x2B: Generic Host Control
    U32 cap;            // Host capability
    U32 ghc;            // Global host control
    U32 is;             // Interrupt status
    U32 pi;             // Port implemented
    U32 vs;             // Version
    U32 ccc_ctl;        // Command completion coalescing control
    U32 ccc_pts;        // Command completion coalescing ports
    U32 em_loc;         // Enclosure management location
    U32 em_ctl;         // Enclosure management control
    U32 cap2;           // Host capabilities extended
    U32 bohc;           // BIOS/OS handoff control and status

    // 0x2C - 0x9F: Reserved
    U8  rsv[0xA0 - 0x2C];

    // 0xA0 - 0xFF: Vendor specific registers
    U8  vendor[0x100 - 0xA0];

    // 0x100 - 0x10FF: Port control registers (array 32 port)
    HBA_PORT ports[32];
} __attribute__((packed)) HBA_MEM;