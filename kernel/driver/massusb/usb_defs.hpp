#pragma once

#include <rosval.h>

struct PACKSTRUCT USBSetupPacket {
    U8  bmRequestType; // 0x80 = Device-to-Host, Standard, Device Recipient
    U8  bRequest;      // 0x06 = GET_DESCRIPTOR
    U16 wValue;        // Descriptor Type (High) & Index (Low). Device = 0x0100
    U16 wIndex;        // 0 or Language ID
    U16 wLength;       // Length of data expected (18 bytes for Device Desc)
};

struct PACKSTRUCT USBDeviceDescriptor {
    U8  bLength;
    U8  bDescriptorType; // Harusnya 0x01
    U16 bcdUSB;
    U8  bDeviceClass;    // INI PENTING: 0x08 = Mass Storage
    U8  bDeviceSubClass;
    U8  bDeviceProtocol;
    U8  bMaxPacketSize0;
    U16 idVendor;
    U16 idProduct;
    U16 bcdDevice;
    U8  iManufacturer;
    U8  iProduct;
    U8  iSerialNumber;
    U8  bNumConfigurations;
};

struct PACKSTRUCT CommandBlockWrapper {
    U32 dCBWSignature;          // Magic: 0x43425355 (Little Endian "USBC")
    U32 dCBWTag;                // ID Unik per command (bebas, asal nanti pas CSW sama)
    U32 dCBWDataTransferLength; // Berapa byte data yang mau dibaca/tulis
    U8  bmCBWFlags;             // 0x80 = Data In (Device to Host), 0x00 = Data Out
    U8  bCBWLUN;                // Logical Unit Number (Biasanya 0)
    U8  bCBWCBLength;           // Panjang perintah SCSI (biasanya 6, 10, 12, atau 16)
    U8  CBWCB[16];              // Command Block (SCSI Command ditaruh disini)
};

// Command Status Wrapper (13 Bytes) - Dikirim Device -> Host (Bulk IN)
struct PACKSTRUCT CommandStatusWrapper {
    U32 dCSWSignature;          // Magic: 0x53425355 (Little Endian "USBS")
    U32 dCSWTag;                // Harus sama dengan dCBWTag yang dikirim
    U32 dCSWDataResidue;        // Data yang GAGAL dikirim/terima
    U8  bCSWStatus;             // 0x00 = Success, 0x01 = Failed, 0x02 = Phase Error
};