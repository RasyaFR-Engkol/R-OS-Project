#pragma once
#include <rosval.h>

// ---------------------------------------------------
// Level 0: Capability Registers (MMIO Base + 0x00)
// ---------------------------------------------------
struct __attribute__((packed)) xHCICapRegisters {
    volatile U8  cap_len;      // Capability Register Length (Offset 0x00)
    volatile U8  rsvd1;
    volatile U16 hci_version;  // Versi HCI (Offset 0x02)
    volatile U32 hcs_params1;  // Structural Parameters 1 (Offset 0x04)
    volatile U32 hcs_params2;  // Structural Parameters 2 (Offset 0x08)
    volatile U32 hcs_params3;  // Structural Parameters 3 (Offset 0x0C)
    volatile U32 hcc_params1;  // Capability Parameters 1 (Offset 0x10)
    volatile U32 dboff;        // Doorbell Offset (Offset 0x14)
    volatile U32 rtsoff;       // Runtime Register Space Offset (Offset 0x18)
    volatile U32 hcc_params2;  // Capability Parameters 2 (Offset 0x1C)
};

// ---------------------------------------------------
// Level 1: Operational Registers (MMIO Base + cap_len)
// ---------------------------------------------------
struct __attribute__((packed)) xHCIOpRegisters {
    volatile U32 usb_cmd;      // USB Command (Offset 0x00)
    volatile U32 usb_sts;      // USB Status (Offset 0x04)
    volatile U32 page_size;    // Page Size (Offset 0x08)
    volatile U32 rsvd1;        // Reserved (Offset 0x0C)
    volatile U32 rsvd1b;       // Reserved (Offset 0x10)
    volatile U32 dn_ctrl;      // Device Notification Control (Offset 0x14)
    volatile U64 crcr;         // Command Ring Control Register (Offset 0x18)
    volatile U64 rsvd2[2];     // Reserved (Offsets 0x20, 0x28)
    volatile U64 dcbaap;       // Device Context Base Address Array Pointer (Offset 0x30)
    volatile U32 config;       // Configure Register (Offset 0x38)
};

// ---------------------------------------------------
// Struct untuk Extended Capability (PENTING!)
// ---------------------------------------------------
struct __attribute__((packed)) xHCIExtCapUSBLegSup {
    volatile U32 leg_sup_cap_id; // ID Capability (Offset 0x00)
    volatile U32 leg_sup_sem;    // Legacy Support Semaphore (Offset 0x04)
};

// ---------------------------------------------------
// Level 2: Runtime Registers (MMIO Base + rtsoff)
// ---------------------------------------------------
struct __attribute__((packed)) xHCIInterrupterRegs {
    volatile U32 iman;     // Interrupt Management
    volatile U32 imod;     // Interrupt Moderation
    volatile U32 erstsz;   // Event Ring Segment Table Size
    volatile U32 rsvd;
    volatile U64 erstba;   // Event Ring Segment Table Base Address
    volatile U64 erdp;     // Event Ring Dequeue Pointer
};

struct __attribute__((packed)) xHCIRuntimeRegisters {
    volatile U32 microframe_index;
    volatile U32 rsvd[7];
    volatile xHCIInterrupterRegs interrupter_regs[1024]; // Array of interrupters
};

// ---------------------------------------------------
// Port Register Set (starting at OpRegs + 0x400)
// ---------------------------------------------------
struct __attribute__((packed)) xHCIPortRegs {
    volatile U32 port_sc;     // Port Status and Control
    volatile U32 port_pmsc;   // Port Power Management Status and Control
    volatile U32 port_li;     // Port Link Info
    volatile U32 port_hlpmc;  // Port Hardware LPM Control
};

// TRB (Transfer Request Block) - 16 bytes
typedef struct __attribute__((packed)) {
    volatile U64 parameter;
    volatile U32 status;
    volatile U32 control;
} xHCITRB;

// ERST (Event Ring Segment Table) Entry - 16 bytes
typedef struct __attribute__((packed)) {
    volatile U64 ring_segment_base_addr; // Alamat Fisik
    volatile U32 ring_segment_size;      // Jumlah TRB
    volatile U32 rsvd;
} xHCIEventRingSegmentTableEntry;