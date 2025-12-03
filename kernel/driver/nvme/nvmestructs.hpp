#pragma once
#include <rosval.h>

namespace NVMe {
    
    // Hasil dari Command Identify (Controller) - 4096 Bytes
    struct NVMeIdentifyControllerStruct {
        U16 VID;    // PCI Vendor ID
        U16 SSVID;  // PCI Subsystem Vendor ID
        U8  SN[20]; // Serial Number (ASCII)
        U8  MN[40]; // Model Number (ASCII)
        U8  FR[8];  // Firmware Revision
        U8  RAB;    // Recommended Arbitration Burst
        U8  IEEE[3];// IEEE OUI Identifier
        U8  CMIC;   // Controller Multi-Path I/O and Namespace Sharing Capabilities
        U8  MDTS;   // Maximum Data Transfer Size
        U16 CNTLID; // Controller ID
        U32 VER;    // Version
        U32 RTD3R;  // RTD3 Resume Latency
        U32 RTD3E;  // RTD3 Entry Latency
        U32 OAES;   // Optional Asynchronous Events Supported
        U32 CTRATT; // Controller Attributes
        U8  Rsvd1[140];
        U8  Rsvd2[240]; // Management interface, etc... (Diskip biar ringkas)
        
        // Total size struct ini aslinya 4096 bytes. 
        // Sisanya padding kalau gak dipake.
        U8  Padding[3584]; 
    } __attribute__((packed));

    // Hasil dari Command Identify (Namespace)
    struct NVMeIdentifyNamespaceStruct {
        U64 NSZE;   // Namespace Size (Total Logical Blocks)
        U64 NCAP;   // Namespace Capacity
        U64 NUSE;   // Namespace Utilization
        U8  NSFEAT; // Namespace Features
        U8  NLBAF;  // Number of LBA Formats
        U8  FLBAS;  // Formatted LBA Size
        
        // ... (sisanya padding, kita cuma butuh Size biasanya)
        U8  Padding[4072];
    } __attribute__((packed));
}