#include "string.hpp"
#include "xhci.hpp"
#include "xhci_internal.hpp"
#include "xhci_regs.hpp"
#include <logging.hpp>
#include <mm.hpp>
#include "../../dev/devicemanager.hpp"
#include "../massusb/usbmsc.hpp"
#include <../kernel/task/reserved/inputdaemon/driver/mouse.hpp>

U8 CalcDCISource(volatile xHCITRB *Event){
    return (Event->control >> 16) & 0x1F; // Ambil Endpoint ID dari TRB event
}

BOOL HandleIfBulkStorage(xHCI::xHCIDriver::XHCIDeviceState &DevState, volatile xHCITRB *Event, U8 CCode, U8 dciSource){
    if(DevState.IsMassStorage){
        UNUSED__ U32 transferred = Event->status & 0x00FFFFFF; // Residual count sebenernya
            // Code '1' = Success, '13' = Short Packet (Masih dianggap sukses buat CSW)
        if (CCode == 1 || CCode == 13) {
            DevState.TransferComplete = TRUE;  
        } else {
            Write(Printk::Level::LOG_ERR, " [MSC] Transfer Failed Code %u DCI %u\n", CCode, dciSource);
            DevState.TransferComplete = TRUE; // Anggap selesai biar gak nunggu terus
        }

        return TRUE;
    } else {
        return FALSE;
    }
}

STATIC VOID CheckAndHandleIfKeyboardHID(xHCI::xHCIDriver::XHCIDeviceState &devState, U8 *data, U32 actualLength){
    if(devState.IsKeyboard && actualLength == 8){
        U8 OldMods = devState.LastKeyboardData[0];
        U8 NewMods = data[0];
        
        // Helper buat Modifier
        auto injectMod = [&](U8 Bitmask, U8 Scancode){
            BOOL OldBit = (OldMods & Bitmask);
            BOOL NewBit = (NewMods & Bitmask);
            if(NewBit && !OldBit) PIC::Keyboard::InjectScancode(Scancode);          // Press
            if(!NewBit && OldBit) PIC::Keyboard::InjectScancode(Scancode | 0x80);   // Release
        };

        BOOL IsChanged = FALSE;
        for(int i=0; i<8; i++) {
            if(devState.LastKeyboardData[i] != data[i]) { IsChanged = TRUE; break; }
        }

        if(IsChanged){
            devState.RepeatCounter = 0;
            devState.RepeatKeyScancode = 0;

            // 1. Handle Modifiers (Ctrl, Shift, Alt)
            injectMod(1, 0x1D); // Ctrl
            injectMod(2, 0x2A); // LShift
            injectMod(4, 0x38); // LAlt

            // 2. Handle Key Press
            for(INTN Key = 2; Key < 8 ; Key++){
                U8 Keycode = data[Key];
                if(Keycode <= 3) continue; 

                BOOL IsNew = TRUE;
                for (int j = 2; j < 8; j++) {
                    if (devState.LastKeyboardData[j] == Keycode) { IsNew = FALSE; break; }
                }

                if(IsNew){
                    U8 PS2 = HID_to_PS2[Keycode];
                    if(PS2 != 0){
                        // kirim prefix 0xE0 jika itu tombol navigasi
                        if(Keycode >= 0x49 && Keycode <= 0x52) {
                            PIC::Keyboard::InjectScancode(0xE0); // Kirim Prefix Make
                        }
                        PIC::Keyboard::InjectScancode(PS2);
                        devState.RepeatKeyScancode = PS2; // Buat auto-repeat
                    }
                }
            }

            // 3. Handle Key Release
            for (int i = 2; i < 8; i++) {
                U8 oldKey = devState.LastKeyboardData[i];
                if (oldKey <= 3) continue;

                bool isReleased = TRUE;
                for (int j = 2; j < 8; j++) {
                    if (data[j] == oldKey) { isReleased = FALSE; break; }
                }

                if (isReleased) {
                    U8 ps2 = HID_to_PS2[oldKey];
                    if (ps2 != 0) {
                        // Jika tombol navigasi (Extended)
                        if(oldKey >= 0x49 && oldKey <= 0x52) {
                            PIC::Keyboard::InjectScancode(0xE0); // Kirim Prefix Break
                        }
                        PIC::Keyboard::InjectScancode(ps2 | 0x80);
                    }
                }
            }
            String::Memcpy(devState.LastKeyboardData, data, 8);
        } else {
            // 4. Logic Auto-Repeat
            if(devState.RepeatKeyScancode != 0){
                devState.RepeatCounter++;
                if (devState.RepeatCounter > 25) { // Threshold repeat
                    devState.RepeatCounter = 23; 
                    PIC::Keyboard::InjectScancode(devState.RepeatKeyScancode);
                }
            }
        }
    }
}

STATIC VOID CheckAndHandleIfMouseHID(xHCI::xHCIDriver::XHCIDeviceState &devState, U8 actualLength, U8 *data){
    if(!devState.IsMouse) return;

    // Mouse Boot Protocol minimal 3 bytes
    if (actualLength >= 3) {
        // Byte 0: Buttons (Bit 0=L, 1=R, 2=M)
        // Byte 1: X (Signed)
        // Byte 2: Y (Signed)

        U8 buttons = data[0];
        I8 x_rel = (I8)data[1];
        I8 y_rel = (I8)data[2];

        // Cek apakah ada perubahan? (Gerak ATAU Klik)
        // Kita butuh simpan last button state di devState buat ngecek perubahan klik
        BOOL moved = (x_rel != 0 || y_rel != 0);
        BOOL clicked = (buttons != devState.LastMouseButtons); // Asumsi kamu nambah variable ini di struct devState

        if(moved || clicked) {
            MouseDriver::SendPacket(x_rel, y_rel, buttons);
            
            // Simpan state
            devState.LastMouseButtons = buttons;
        }
    }
}

VOID HandleIfHIDInput(xHCI::xHCIDriver::XHCIDeviceState &DevState, volatile xHCITRB *Event, U8 CCode, U8 dciSource){
    U8 *Data = DevState.IntBufferVirt;
    
    // === FIX MATEMATIKA ===
    // Kita harus tahu berapa yang KITA MINTA di ISR tadi.
    // Logic ini harus SAMA PERSIS dengan xhci_isr.cpp saat QueueInterruptTransfer
    
    U32 LengthRequested = 64; 
    
    U32 Residual = (Event->status & 0x00FFFFFF);

    // Kalau residual > request, baru itu error aneh.
    if (Residual > LengthRequested) Residual = LengthRequested;

    U32 ActualLength = LengthRequested - Residual;

    // DEBUG JITULAH
    if(ActualLength > 0 && DevState.IsMouse) {
        /*Serial::Printf(" [MOUSE] Bytes: %d | Data: %02X %02X %02X ...\n", 
            ActualLength, DevState.IntBufferVirt[0], DevState.IntBufferVirt[1], DevState.IntBufferVirt[2]);*/
    }

    // Debugging print
    //if(DevState.IsMouse) Serial::Printf("Mouse Req: %d Res: %d Actual: %d\n", LengthRequested, Residual, ActualLength);
    //if(DevState.IsKeyboard) Serial::Printf("Keyboard Req: %d Res: %d Actual: %d\n", LengthRequested, Residual, ActualLength);

    if (DevState.IsKeyboard) {
        CheckAndHandleIfKeyboardHID(DevState, Data, ActualLength);
    } 
    else if (DevState.IsMouse) {
        CheckAndHandleIfMouseHID(DevState, ActualLength, Data);
    }
}

VOID FindClassAndEndpoint(U32 &offset, U16 &totalLen, U8 *buffer, xHCI::xHCIDriver &DRV, U8 SlotID, BOOL &found, U8 &currentInterfaceClass) {
    
    while (offset < totalLen) {
        U8 len = buffer[offset];
        U8 type = buffer[offset + 1];
        
        // Safety check biar gak infinite loop kalau buffer corrupt (len 0)
        if (len == 0) break;

        // ==========================================
        // TYPE 5: ENDPOINT DESCRIPTOR
        // ==========================================
        if (type == USB_DESC_TYPE_ENDPOINT) { 
            U8 addr = buffer[offset + 2];
            U8 attr = buffer[offset + 3];
            U16 pkt = buffer[offset + 4] | (buffer[offset + 5] << 8);
            U8 interval = buffer[offset + 6];

            U8 EpNum = addr & 0xF;
            U8 DirIn = (addr & 0x80) ? 1 : 0;
            U8 TransferType = attr & 0x3;

            switch (currentInterfaceClass) {
                // --- HID (Mouse/Keyboard) ---
                case USB_CLASS_HID: {
                    if (TransferType == USB_EP_ATTR_TYPE_INTERRUPT && DirIn) {
                        if (pkt < 8) pkt = 8;

                        //Write(Printk::Level::LOG_NOTICE, "   >>> FOUND INPUT ENDPOINT! DCI=%u (Addr=0x%x) MPS=%u\n", ((addr & 0xF) * 2) + 1, addr, pkt);

                        DRV.Devs[SlotID].ActiveIntDCI = (EpNum * 2) + 1;
                        
                        // === TAMBAHAN ===
                        DRV.Devs[SlotID].IntMaxPacketSize = pkt; // Simpan Packet Size asli dari device!
                        // ================

                        ConfigureEndpoint(DRV, SlotID, addr, 7, pkt, interval);
                        found = TRUE;
                    }
                    break;
                }

                // --- Mass Storage (Flashdisk) ---
                case USB_CLASS_MASS_STORAGE: {
                    if (TransferType == USB_EP_ATTR_TYPE_BULK) { // Bulk
                        U8 DCI = (EpNum * 2) + DirIn;

                        if (DirIn) {
                            Printk::Write(Printk::Level::LOG_NOTICE, "   >>> FOUND MSC BULK IN! DCI=%u MPS=%u\n", DCI, pkt);
                            DRV.Devs[SlotID].BulkInDCI = DCI;
                        } else {
                            Printk::Write(Printk::Level::LOG_NOTICE, "   >>> FOUND MSC BULK OUT! DCI=%u MPS=%u\n", DCI, pkt);
                            DRV.Devs[SlotID].BulkOutDCI = DCI;
                        }

                        xHCI::ConfigureEndpoint(DRV, SlotID, addr, 0, pkt, interval);
                    }
                    break;
                }

                // --- USB HUB (Status Change) ---
                case USB_CLASS_HUB: {
                    if (TransferType == USB_EP_ATTR_TYPE_INTERRUPT && DirIn) { // Interrupt IN
                        //Write(Printk::Level::LOG_NOTICE, "   >>> FOUND HUB STATUS ENDPOINT! DCI=%u MPS=%u Interval=%u\n",
                        //      ((addr & 0xF) * 2) + 1, pkt, interval);

                        DRV.Devs[SlotID].ActiveIntDCI = (EpNum * 2) + 1;
                        ConfigureEndpoint(DRV, SlotID, addr, 7, pkt, interval);
                        found = TRUE;
                    }
                    break;
                }

                default: {
                    break;
                }
            }
        } 
        // ==========================================
        // TYPE 4: INTERFACE DESCRIPTOR
        // ==========================================
        else if (type == USB_DESC_TYPE_INTERFACE) {
            U8 interfaceClass = buffer[offset + 5];
            U8 interfaceSubClass = buffer[offset + 6];
            U8 interfaceProtocol = buffer[offset + 7];

            // Update state class saat ini
            currentInterfaceClass = interfaceClass;

            switch (interfaceClass) {
                case USB_CLASS_HID: {
                    //Write(Printk::Level::LOG_NOTICE, "   [DETECT] Found HID Device (Mouse/Keyboard)!\n");
                    if (interfaceProtocol == HID_PROTOCOL_KEYBOARD) {
                       // Write(Printk::Level::LOG_NOTICE, "   [DETECT] Found HID Keyboard!\n");
                        DRV.Devs[SlotID].IsKeyboard = TRUE;
                        DRV.Devs[SlotID].IsMouse = FALSE;
                    } else if (interfaceProtocol == HID_PROTOCOL_MOUSE) {
                       // Write(Printk::Level::LOG_NOTICE, "   [DETECT] Found HID Mouse!\n");
                        DRV.Devs[SlotID].IsMouse = TRUE;
                        DRV.Devs[SlotID].IsKeyboard = FALSE;
                    } else {
                       // Write(Printk::Level::LOG_NOTICE, "   [DETECT] Found Generic HID (Joystick/Tablet) Protocol: %d\n", interfaceProtocol);
                    }
                    break;
                }

                case USB_CLASS_MASS_STORAGE: {
                    if (interfaceSubClass == 0x06 && interfaceProtocol == 0x50) {
                        Write(Printk::Level::LOG_NOTICE, "   [DETECT] Found Mass Storage Device (SCSI/Bulk-Only)!\n");
                        DRV.Devs[SlotID].IsMassStorage = TRUE;
                    }
                    break;
                }

                case USB_CLASS_HUB: {
                    Write(Printk::Level::LOG_NOTICE, "   [DETECT] Found HUB Device!\n");
                    DRV.Devs[SlotID].IsHub = TRUE;
                    break;
                }

                default:
                    Write(Printk::Level::LOG_NOTICE, "   [DETECT] Found Unknown Interface Class: 0x%02X\n", interfaceClass);
                    break;
            }
        }

        // ==========================================
        // NEXT DESCRIPTOR (CRUCIAL!)
        // ==========================================
        // Posisinya harus sejajar dengan IF/ELSE IF, 
        // tapi di dalam WHILE.
        offset += len;
    }
}

VOID ResetDevState(xHCI::xHCIDriver &DRV, U32 SlotID){
    DRV.Devs[SlotID].ActiveIntDCI = 0;
    DRV.Devs[SlotID].BulkInDCI = 0;
    DRV.Devs[SlotID].BulkOutDCI = 0;
    DRV.Devs[SlotID].IsMassStorage = FALSE;
    DRV.Devs[SlotID].IsKeyboard = FALSE; // Reset flag keyboard juga
    DRV.Devs[SlotID].IsMouse = FALSE;    // Reset flag mouse juga
    DRV.Devs[SlotID].LastMouseButtons = 0;
    DRV.Devs[SlotID].RepeatCounter = 0;
    DRV.Devs[SlotID].RepeatKeyScancode = 0;
    DRV.Devs[SlotID].IsHub = FALSE;
}

VOID FreeDeviceResources(xHCI::xHCIDriver &DRV, U32 SlotID){
    auto &dev = DRV.Devs[SlotID];

    Write(Printk::Level::LOG_INFO, " xHCI: Freeing resources for Slot %u...\n", (unsigned)SlotID);

    // 1. Free EP0 Ring
    if(dev.EP0Ring){
        PageAlloc::DMAAlloc::FreeDMABuffer(dev.EP0Ring);
        dev.EP0Ring = nullptr;
    }

    // 2. Free Interrupt Buffer (Mouse/Keyboard data buffer)
    if(dev.IntBufferDMA){
        PageAlloc::DMAAlloc::FreeDMABuffer(dev.IntBufferDMA);
        dev.IntBufferDMA = nullptr;
        dev.IntBufferPhys = 0;
        dev.IntBufferVirt = nullptr;
    }

    // 3. Free Endpoint Rings (Loop semua endpoint 1-31)
    for(int i=0; i<32; i++){
        if(dev.Endpoints[i].Ring){
            PageAlloc::DMAAlloc::FreeDMABuffer(dev.Endpoints[i].Ring);
            dev.Endpoints[i].Ring = nullptr;
            dev.Endpoints[i].EnqueueIdx = 0;
        }
    }

    U64 devCtxPhys = DRV.V_DCBAAP[SlotID];
    if(devCtxPhys != 0) {
        // Null-kan dulu di controller & flush
        DRV.V_DCBAAP[SlotID] = 0;
        asm volatile ("clflush (%0)" :: "r"(&DRV.V_DCBAAP[SlotID]) : "memory");
        asm volatile ("mfence" ::: "memory");

        // Free pakai PhysicalFreePages (1 Page)
        PageAlloc::PhysicalFreePages(devCtxPhys, 1);
    }

    // 4. Free Device Context DMA Buffer
    if(dev.InputContextPhys){
        PageAlloc::PhysicalFreePages(dev.InputContextPhys, 1); // Asumsi 1 page
        dev.InputContextPhys = 0;
    }
    
    // Tapi kalau pake cara sekarang, pastikan kamu punya fungsi FreePhysicalPages(physAddr):
    if (DRV.V_DCBAAP[SlotID]) {
        // PageAlloc::FreePhysicalPages(DRV.V_DCBAAP[SlotID], 1); // Asumsi ada fungsi ini
        DRV.V_DCBAAP[SlotID] = 0; // Null-kan entri di DCBAAP
    }
}

VOID xHCI::CheckPendingMSC(xHCIDriver &DRV){
    // Loop cek semua slot, ada yang minta di-init gak?
        // (Optimasi: Bisa batasi loop sampai MAX_SLOTS atau pakai list)
        for (U32 i = 1; i < xHCIDriver::MAX_SLOTS; i++) {
            if (DRV.Devs[i].PendingMSCInit) {
                // Reset flag biar gak dipanggil berkali-kali
                DRV.Devs[i].PendingMSCInit = FALSE;

                Printk::Write(Printk::Level::LOG_NOTICE, " xHCI: Defer-Initializing MSC for Slot %u...\n", i);

                // Ambil info endpoints
                U8 inDCI = DRV.Devs[i].BulkInDCI;
                U8 outDCI = DRV.Devs[i].BulkOutDCI;

                if (inDCI && outDCI) {
                    // Create Driver
                    auto *mscDriver = new USBMassStorage(&DRV, (U8)i, inDCI, outDCI);
                    
                    if (mscDriver) {
                        // Initialize (Sekarang aman karena Interrupt Enabled!)
                        if (mscDriver->Initialize()) {
                            // Register ke Device Manager
                            if (DeviceManager::RegisterBlockDevice(mscDriver)) {
                                Printk::Write(Printk::Level::LOG_NOTICE, " xHCI: USB Disk Registered Successfully! (/dev/USBDisk...)\n");
                                
                                // Opsional: Panggil GPTFS::InitFs() lagi buat scan partisi baru
                                // GPTFS::InitFs(); 
                            } else {
                                Printk::Write(Printk::Level::LOG_ERR, " xHCI: Failed to register MSC device.\n");
                            }
                        } else {
                            Printk::Write(Printk::Level::LOG_ERR, " xHCI: MSC Initialization Failed.\n");
                            delete mscDriver;
                        }
                    }
                }
            }
        }
}