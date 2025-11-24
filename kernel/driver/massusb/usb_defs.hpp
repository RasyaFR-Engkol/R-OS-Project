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