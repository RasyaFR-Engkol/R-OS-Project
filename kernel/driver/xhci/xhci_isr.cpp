#define PRINTK_MODULE_NAME "XHCIIntr"
#include <rosval.h>
#include <rossys.hpp>
#include <logging.hpp>
#include <mm.hpp>
#include "string.hpp"
#include "xhci.hpp"
#include "xhci_regs.hpp"
#include "xhci_internal.hpp"
#include "../massusb/usb_defs.hpp"
#include "../massusb/usbmsc.hpp"
#include "../../dev/devicemanager.hpp"

namespace xHCI{
    using namespace Printk;

    // Map completed SlotID back to the root port that initiated the EnableSlot.
    // Strategy:
    // 1) Prefer ports that we marked as PORT_STATE_ENABLE_SENT when issuing
    //    the EnableSlot command (common path).
    // 2) Fallback: attempt to read the Slot Context RootPort from the DCBAAP
    //    entry if present.
    U8 GetPortIDForSlot(xHCIDriver &DRV, U8 SlotID) {
        // Search ports for the one in ENABLE_SENT state
        for (U32 p = 0; p < DRV.PortCount; ++p) {
            if (DRV.PortStates[p].State == xHCIDriver::PORT_STATE_ENABLE_SENT) {
                // Mark it as addressing to avoid reuse
                DRV.PortStates[p].State = xHCIDriver::PORT_STATE_ADDRESSING;
                return (U8)(p + 1); // Port IDs are 1-based
            }
        }

        // Fallback: try to read root port from the device context array
        if (DRV.V_DCBAAP) {
            U64 dcba_phys = (U64)DRV.V_DCBAAP[SlotID];
            if (dcba_phys) {
                // DCBAAP points to a device context base address (phys). Slot Context
                // is at offset 32 bytes into the device context structure.
                volatile U32 *slotCtx = (volatile U32*)HHDM_PhysToVirt((UPTR)dcba_phys + 32);
                // The RootPort is stored in SlotContext[1] bits 16..23 per earlier usage
                U32 slotCtx1 = slotCtx[1];
                U8 rootPort = (U8)((slotCtx1 >> 16) & 0xFF);
                if (rootPort != 0) {
                    Write(Level::LOG_DEBUG, " GetPortIDForSlot: derived rootPort %u from DCBAAP for slot %u\n", (unsigned)rootPort, (unsigned)SlotID);
                    return rootPort;
                }
            }
        }

        Write(Level::LOG_WARNING, " GetPortIDForSlot: Unable to find port for Slot %u; defaulting to 1\n", (unsigned)SlotID);
        return 0;
    }

    static VOID HandlePortStatusChange(xHCIDriver &DRV, U8 PortID){
        if(PortID == 0 || PortID > DRV.PortCount) return;

        VOLATILE xHCIPortRegs *PortReg = &DRV.port_regs[PortID - 1];
        U32 PortSC = PortReg->port_sc;

        Write(Level::LOG_INFO, " Port %u Event - PortSC=0x%08x\n", (unsigned)PortID, (unsigned)PortSC);

        // ====================================================
        // 1. EVENT: CONNECT STATUS CHANGE (Baru Dicolok)
        // ====================================================
        if(PortSC & (1 << 17)) { // CSC
            // Clear CSC (Write-1-to-Clear)
            PortReg->port_sc = (PortSC & ~0x00FE0000) | (1 << 17);
            
            // Kalau Connected tapi belum Enabled, berarti butuh RESET
            if(PortSC & 1){
                // Kasus A: Connected tapi belom enabled
                if((PortSC & 2) == 0){
                    Write(Level::LOG_NOTICE, " xHCI: Port %u Connected (USB 2.0 style). Initiating Reset...\n", (unsigned)PortID);
                    
                    U32 ResetCMD = PortSC;
                    ResetCMD &= ~0x00FE0000; 
                    ResetCMD |= (1 << 4);    // Set PR (Port Reset)
                    ResetCMD &= ~(1 << 1);   // Clear PED
                    PortReg->port_sc = ResetCMD;
                    return; // Tunggu interrupt PRC (Port Reset Change) nanti
                }

                // KASUS B: Connected DAN sudah Enabled (Biasanya USB 3.0)
                // Langsung gas ke Enable Slot!
                
                else {
                    Write(Level::LOG_NOTICE, " xHCI: Port %u Connected & Enabled (USB 3.0 style). Skip Reset.\n", (unsigned)PortID);
                    
                    // Kita bisa langsung anggap ini setara dengan Reset Complete
                    // Lanjut ke alokasi slot
                     DRV.PortStates[PortID - 1].State = xHCIDriver::PORT_STATE_ENABLE_SENT;
                     SendEnableSlotCommand(DRV);
                }
            } else {
                // kasus C: Disconnected Devices

                Write(Level::LOG_NOTICE, " xHCI: Port %u Disconnected. Cleaning up device...\n", (unsigned)PortID);

                U8 SlotID = DRV.PortStates[PortID - 1].SlotID;
                if(SlotID != 0){
                    Write(Level::LOG_INFO, " xHCI: Port %u - Removing device at Slot %u due to disconnect.\n", (unsigned)PortID, (unsigned)SlotID);
                    U32 CMDIDX = DRV.CmdRingEnqueueIndex;
                    VOLATILE xHCITRB *CmdTRB = &DRV.VCmdRing[CMDIDX];

                    CmdTRB->parameter = 0;
                    CmdTRB->status = 0;
                    CmdTRB->control = (10u << 10) | // TRB_TYPE_DISABLE_SLOT
                                      (SlotID << 24) |
                                      (DRV.CmdRingCycleState ? 1u : 0u);

                    DRV.doorbell_regs[0] = 0; // Ring doorbell for command ring

                    DRV.CmdRingEnqueueIndex++;
                    if(DRV.CmdRingEnqueueIndex == DRV.CmdRingSize - 1){
                        // Handle Link TRB Wrap (sama kayak logic kamu biasanya)
                        volatile xHCITRB *LinkTRB = &DRV.VCmdRing[DRV.CmdRingEnqueueIndex];
                        LinkTRB->control = (6U << 10) | (1U << 1) | (DRV.CmdRingCycleState ? 1u : 0u);
                        DRV.CmdRingEnqueueIndex = 0;
                        DRV.CmdRingCycleState = !DRV.CmdRingCycleState;
                    }

                    FreeDeviceResources(DRV, SlotID);

                    ResetDevState(DRV, SlotID);

                    DRV.PortStates[PortID - 1].State = xHCIDriver::PORT_STATE_EMPTY;
                    DRV.PortStates[PortID - 1].SlotID = 0;
                }
            }
        }

        // ====================================================
        // 2. EVENT: PORT RESET CHANGE (Reset Selesai)
        // ====================================================
        if(PortSC & (1 << 21)) { // PRC (Port Reset Change)
            // Clear PRC (Write-1-to-Clear)
            PortReg->port_sc = (PortSC & ~0x00FE0000) | (1 << 21);
            
            Write(Level::LOG_NOTICE, " xHCI: Port %u Reset Complete (PRC). Sending Enable Slot...\n", (unsigned)PortID);

            // Tandai state port
            DRV.PortStates[PortID - 1].State = xHCIDriver::PORT_STATE_ENABLE_SENT;

            // Kirim Command Enable Slot
            SendEnableSlotCommand(DRV);
        }

        // sampe sini? biasanya kita nggak tau ada apaan
        Printk::Write(Printk::Level::LOG_DEBUG, " xHCI: Port %u - No recognized status change handled.\n", (unsigned)PortID);
    }

    static VOID ProcessPendingEvents(xHCIDriver &DRV, U32 Controller_ID){
        volatile xHCIInterrupterRegs *IR0 = &DRV.rt_regs->interrupter_regs[0];

        bool didWork = false; // Flag penanda: Ada kerjaan gak?

        while(TRUE){
            U32 index = DRV.EventRingDequeueIndex;
            volatile xHCITRB *Event = &DRV.VEventRing[index];

            asm volatile("clflush (%0)" :: "r"(Event) : "memory");
            asm volatile("mfence" ::: "memory"); // Tunggu memori sinkron

            U32 control = Event->control;

            if ((control & 1u) != (DRV.EventRingCycleState ? 1u : 0u)) {
                break;
            }

            didWork = true;

            U8 EventType = (U8)((control >> 10) & 0x3Fu);
            Serial::Printf(" xHCI: Controller %u - Event Type %u detected (param=0x%016llx status=0x%08x ctl=0x%08x)\n",
                (unsigned)Controller_ID, (unsigned)EventType,
                (unsigned long long)Event->parameter,
                (unsigned)Event->status,
                (unsigned)Event->control);

            // 33 = Command Completion Event, 34 = Port Status Change Event
            if(EventType == 33){
                U8  ccode   = (U8)(Event->status >> 24);
                U8  slotId  = (U8)(Event->control >> 24);
                U64 cmdTrbPhys = Event->parameter; // Pointer ke Command TRB asli
                
                // Kita harus intip Command TRB aslinya untuk tahu ini command apa
                volatile xHCITRB *OriginalCmd = (volatile xHCITRB*)HHDM_PhysToVirt((UPTR)cmdTrbPhys);
                U32 CmdType = (OriginalCmd->control >> 10) & 0x3F; // Ambil TRB Type dari Command aslinya

                if(ccode == 1){ // Success
                    
                    switch(CmdType) {
                        case 23: // TRB_TYPE_NOOP
                            Write(Level::LOG_DEBUG, " xHCI: NOOP Command Completed.\n");
                            break;

                        case 9: // TRB_TYPE_ENABLE_SLOT
                            {
                                // Cari port mana yang request enable slot ini
                                // (Logic loop portWaitingEnable kamu pindah kesini)
                                U32 targetPort = 0;
                                for(U32 p=0; p<DRV.PortCount; p++){
                                    if(DRV.PortStates[p].State == xHCIDriver::PORT_STATE_ENABLE_SENT){
                                        targetPort = p + 1;
                                        DRV.PortStates[p].State = xHCIDriver::PORT_STATE_ADDRESSING; 
                                        break;
                                    }
                                }
                                
                                if(targetPort) {
                                    Write(Level::LOG_INFO, " xHCI: Slot ID %u assigned to Port %u. Sending Address Device...\n", slotId, targetPort);
                                    // PENTING: Simpan SlotID ini ke struktur Port atau Device array kamu
                                    DRV.PortStates[targetPort-1].SlotID = slotId; 
                                    
                                    SetupAddressDevice(DRV, slotId, targetPort);
                                } else {
                                    Write(Level::LOG_WARNING, " xHCI: Enable Slot completed but no port was waiting?\n");
                                }
                            }
                            break;

                        case 11: // TRB_TYPE_ADDRESS_DEVICE
                            Write(Level::LOG_INFO, " xHCI: Address Device Command Completed for Slot %u.\n", slotId);
                            // Baru disini aman panggil GetDescriptor
                            GetDeviceDescriptor(DRV, slotId);
                            break;

                        case 12: // TRB_TYPE_CONFIGURE_ENDPOINT
                        {
                            Write(Level::LOG_INFO, " xHCI: Configure Endpoint Completed for Slot %u. Endpoint READY!\n", slotId);

                            DRV.Devs[slotId].Stage = xHCIDriver::XHCIDeviceState::STAGE_RUNNING;

                            // 2. Siapkan Buffer
                            // === FIX ALOKASI MEMORI ===
                            // JANGAN 8 BYTE! Alokasi 64 byte biar aman buat Mouse Gaming.
                            // Keyboard pake 8 byte dari 64 byte ini gak masalah.
                            if(DRV.Devs[slotId].IntBufferVirt == nullptr) {
                                // Alokasi 64 byte (MaxPacketSize USB 2.0 Int)
                                PageAlloc::DMAAlloc::DMABuffer* intBuf = PageAlloc::DMAAlloc::AllocateDMABytes(64);
                                DRV.Devs[slotId].IntBufferDMA = intBuf;
                                DRV.Devs[slotId].IntBufferPhys = intBuf->PhysAddr;
                                DRV.Devs[slotId].IntBufferVirt = (U8*)intBuf->VirtAddr;
                            }

                            // 3. Tentukan Transfer Length
                            // === FIX LOGIC TRB ===
                            U32 transferLen = 8; // Default Keyboard
                            if(DRV.Devs[slotId].IsMassStorage){
                                Write(Level::LOG_DEBUG, " xHCI: Mass Storage Device detected, mounting.\n");
                                
                                DRV.Devs[slotId].PendingMSCInit = TRUE;
                            } else if(DRV.Devs[slotId].IsMouse || DRV.Devs[slotId].IsKeyboard){
                                if (DRV.Devs[slotId].IsMouse) {
                                    transferLen = 8; // Atau lebih besar
                                    Write(Level::LOG_DEBUG, " xHCI: Queueing first MOUSE Transfer for Slot %u\n", slotId);
                                } else {
                                    Write(Level::LOG_DEBUG, " xHCI: Queueing first KEYBOARD Transfer for Slot %u\n", slotId);
                                }

                                U8 targetDCI = DRV.Devs[slotId].ActiveIntDCI;
                                if(targetDCI == 0) targetDCI = 3; 

                                QueueInterruptTransfer(DRV, slotId, targetDCI, DRV.Devs[slotId].IntBufferPhys, transferLen);

                                // 5. Update Doorbell
                                DRV.doorbell_regs[slotId] = targetDCI; 
                            } else {
                                Write(Printk::Level::LOG_ALERT, "unknown devices. no queue \n");
                            }
                        }
                        break;
                        case 13: //TRB_TYPE_CONFIGURE_ENDPOINT
                            {
                                Write(Level::LOG_INFO, " xHCI: Configure Endpoint Completed for Slot %u. Endpoint READY!\n", slotId);

                                // 1. Siapkan Buffer untuk terima data Mouse (misal 8 byte cukup untuk Boot Protocol)
                                // Simpan pointer ini di struct supaya nanti pas interrupt (STAGE_RUNNING) bisa dibaca datanya
                                PageAlloc::DMAAlloc::DMABuffer* intBuf = PageAlloc::DMAAlloc::AllocateDMABytes(8);
                                DRV.Devs[slotId].IntBufferPhys = intBuf->PhysAddr;
                                DRV.Devs[slotId].IntBufferVirt = (U8*)intBuf->VirtAddr; // Pastikan member ini ada

                                // 2. Ambil DCI yang tadi kita simpan pas parsing
                                U8 targetDCI = DRV.Devs[slotId].ActiveIntDCI;
                                if(targetDCI == 0) targetDCI = 3; // Fallback kalau lupa simpan (EP 1 IN)

                                // 3. Masukkan Transfer TRB ke Ring Endpoint (bukan Command Ring!)
                                // "Tolong isi buffer ini kalau ada data dari mouse"
                                if(DRV.Devs[slotId].IsMouse) {
                                    Write(Level::LOG_DEBUG, " xHCI: Queueing first Interrupt Transfer for Mouse Slot %u DCI %u\n", slotId, targetDCI);
                                    QueueInterruptTransfer(DRV, slotId, targetDCI, intBuf->PhysAddr, 8);
                                } else if(DRV.Devs[slotId].IsKeyboard) {
                                    Write(Level::LOG_DEBUG, " xHCI: Queueing first Interrupt Transfer for Keyboard Slot %u DCI %u\n", slotId, targetDCI);
                                    QueueInterruptTransfer(DRV, slotId, targetDCI, intBuf->PhysAddr, 8);
                                } else {
                                    Write(Level::LOG_DEBUG, " xHCI: Queueing first Interrupt Transfer for HID Slot %u DCI %u\n", slotId, targetDCI);
                                }

                                // 4. Update Doorbell! (PENTING)
                                // Tanpa ini, xHCI gak akan sadar ada TRB baru di Endpoint Ring
                                DRV.doorbell_regs[slotId] = targetDCI; 

                                // 5. Update State
                                DRV.Devs[slotId].Stage = xHCIDriver::XHCIDeviceState::STAGE_RUNNING;
                                
                                Write(Level::LOG_NOTICE, " xHCI: Slot %u is now RUNNING! Waiting for mouse input...\n", slotId);
                            }
                            break;

                            case 10: // TRB_TYPE_DISABLE_SLOT
                                Write(Level::LOG_INFO, " xHCI: Disable Slot Command Completed for Slot %u.\n", slotId);
                                // Slot sudah disabled di hardware. 
                                // Resource software (memori) udah kita free di event disconnect sebelumnya.
                                // Jadi di sini sebenernya nothing to do, cuma acknowledge aja.
                                break;

                        default:
                            Write(Level::LOG_DEBUG, " xHCI: Unknown Command %u Completed for Slot %u.\n", CmdType, slotId);
                            break;
                    }

                } else {
                    // Handle Error (ccode != 1)
                    Write(Level::LOG_ERR, " xHCI: Command Type %u Failed! Code: %u Slot: %u\n", CmdType, ccode, slotId);
                }
            } else if (EventType == 32){ // Transfer Event
                U8 CCode = (U8)(Event->status >> 24);
                U8 SlotID = (U8)(Event->control >> 24);
                U8 dciSource = CalcDCISource(Event);
                auto &devState = DRV.Devs[SlotID];

                Serial::Printf( " [DEBUG] ISR Read Stage %d for Slot %u (Addr: 0x%016llx)\n", 
                    (int)devState.Stage, (unsigned)SlotID, (unsigned long long)&devState.Stage);

                if(CCode == 1 || CCode == 13){ // Success
                    if(devState.IsMassStorage){
                         if(HandleIfBulkStorage(devState, Event, CCode, dciSource)){
                             // Event sudah dihandle MSC, jangan lanjut ke switch stage/HID
                             // Lanjut ke update dequeue pointer
                             goto finish_event;
                         }
                    }

                    // CEK STAGE SEKARANG APA?
                    switch(devState.Stage) {
                        case xHCIDriver::XHCIDeviceState::STAGE_GET_DESCRIPTOR_SENT:
                            Write(Level::LOG_INFO, " xHCI: Get Device Descriptor DONE for Slot %u\n", SlotID);
                            
                            // Parse Descriptor (ambil VID/PID)
                            // ... kode parsing descriptor kamu ...
                            
                            // Lanjut ke Next Step
                            SetDeviceConfiguration(DRV, SlotID, 1);
                            break;

                        case xHCIDriver::XHCIDeviceState::STAGE_SET_CONFIG_SENT:
                            Write(Level::LOG_INFO, " xHCI: Set Configuration DONE for Slot %u\n", SlotID);

                            {
                                // Alokasi buffer uncached buat nampung descriptor
                                PageAlloc::DMAAlloc::DMABuffer* descBuf = PageAlloc::DMAAlloc::AllocateDMABytes(256);
                                DRV.Devs[SlotID].LastEP0DestPhys = descBuf->PhysAddr; // Simpan pointer biar bisa dibaca pas interrupt
                                DRV.Devs[SlotID].Stage = xHCIDriver::XHCIDeviceState::STAGE_GET_CONFIG_DESC_SENT;
                                
                                // Request Config Descriptor (Type 2), Index 0, Length 256
                                GetDescriptor(DRV, SlotID, 2, 0, 256, descBuf->PhysAddr);
                            }
                            break;

                        case xHCIDriver::XHCIDeviceState::STAGE_GET_CONFIG_DESC_SENT:
                        {
                            // Cek apakah ini event duplikat (Status Stage)?
                            // Kita bisa cek apakah kita baru saja memproses ini.
                            // Tapi cara paling gampang: Ubah stage langsung setelah sukses.
                            
                            Write(Level::LOG_INFO, " xHCI: Config Descriptor Received! Parsing...\n");
                            
                            U64 bufPhys = DRV.Devs[SlotID].LastEP0DestPhys;
                            U8* buffer = (U8*)HHDM_PhysToVirt(bufPhys); 
                            U16 totalLen = buffer[2] | (buffer[3] << 8); 
                            if (totalLen > 1024) totalLen = 1024; 

                            U32 offset = 0;
                            UNUSED__ BOOL found = FALSE;

                            ResetDevState(DRV, SlotID);

                            UNUSED__ U8 currentInterfaceClass = 0;

                            FindClassAndEndpoint(offset, totalLen, buffer, DRV, SlotID, found, currentInterfaceClass);

                            // Pindah Stage
                            // Kalau MSC, kita tunggu command selanjutnya (SCSI), gak perlu pancing interrupt.
                            if (DRV.Devs[SlotID].IsMassStorage) {
                                if (DRV.Devs[SlotID].BulkInDCI && DRV.Devs[SlotID].BulkOutDCI) {
                                    DRV.Devs[SlotID].Stage = xHCIDriver::XHCIDeviceState::STAGE_RUNNING;
                                    Write(Level::LOG_NOTICE, "   Mass Storage Configured! Ready for SCSI Commands.\n");
                                } else {
                                    Write(Level::LOG_ERR, "   Mass Storage Error: Missing Endpoints!\n");
                                }
                            } else {
                                // Flow lama buat Mouse/Keyboard
                                DRV.Devs[SlotID].Stage = xHCIDriver::XHCIDeviceState::STAGE_ENDPOINT_CONFIG_SENT;
                            }
                        }
                        break;

                        
                        case xHCIDriver::XHCIDeviceState::STAGE_CONFIGURED:
                            // Ini mungkin interrupt dari transfer data mouse/keyboard biasa
                            // Handle data endpoint disini nanti
                            break;

                        case xHCIDriver::XHCIDeviceState::STAGE_RUNNING:
                        {

                            Serial::Printf(" [DEBUG] ISR Stage RUNNING for Slot %u DCI Source %u\n", (unsigned)SlotID, (unsigned)dciSource);


                            /**
                             * Berarti ini adalah HID Device (Mouse/Keyboard)
                             */

                            HandleIfHIDInput(devState, Event, CCode, dciSource);

                            // ===============================================
                            // KITA HARUS SELALU RE-QUEUE, GAK PEDULI CODE 1 ATAU 13
                            // ===============================================
                            U8 dci = devState.ActiveIntDCI ? devState.ActiveIntDCI : 3;
                            if (dciSource == dci) { 
                                U32 transferLen = 8;    
                                QueueInterruptTransfer(DRV, SlotID, dci, devState.IntBufferPhys, transferLen);
                            }
                        }
                        break;
                                        
                        default:
                            Write(Level::LOG_WARNING, " xHCI: Unknown Transfer Event for Slot %u in Stage %d\n", SlotID, devState.Stage);
                            break;
                    }
                } 
                else {
                    // Nah, kalau ini baru error beneran (selain 1 dan 13)
                    U32 Residual = (Event->status & 0x00FFFFFF);
                    UNUSED__ U32 TRBLen = (Event->control >> 16) & 0xFFFF; // (Tergantung format TRB event controller, kadang ga valid)
                    
                    Printk::Write(Printk::Level::LOG_ERR, " [XFER FAIL] Slot %u Code %d (Babble/Error)\n", SlotID, CCode);
                    Printk::Write(Printk::Level::LOG_ERR, "     Residual: %u bytes (Sisa space di buffer)\n", Residual);
                    
                    if (CCode == 3) {
                        Printk::Write(Printk::Level::LOG_ERR, "     BABBLE DETECTED! Device ngirim data lebih banyak dari TRB Length!\n");
                        Printk::Write(Printk::Level::LOG_ERR, "     Solusi: Gedein transfer length di QueueInterruptTransfer (min 64 buat mouse gaming)\n");
                    }
                }
            } else if (EventType == 34) {
                U8 PortID = (U8)((Event->parameter >> 24) & 0xFF);

                Write(Level::LOG_NOTICE, " xHCI: Port Status Change detected on Port %d\n", PortID);

                HandlePortStatusChange(DRV, PortID);
            } else {
                Write(Level::LOG_INFO, " Controller %u - Event Type %u (param=0x%016llx status=0x%08x ctl=0x%08x)\n",
                    (unsigned)Controller_ID, (unsigned)EventType,
                    (unsigned long long)Event->parameter,
                    (unsigned)Event->status,
                    (unsigned)Event->control);
            }

            finish_event:

            index++;
            if(index == DRV.EventRingSize){
                index = 0;
                DRV.EventRingCycleState = !DRV.EventRingCycleState;
            }

            DRV.EventRingDequeueIndex = index;
        }

        if (!didWork) {
             DRV.SpuriousInterruptCount++;
             // Print cuma kalo sering banget (biar gak nyampah) atau buat debug awal
             Printk::Write(Printk::Level::LOG_DEBUG, " xHCI: Spurious Interrupt (No events found)\n");
        } else {
             // Kalau didWork = true, berarti Interrupt Valid. Jangan print "Spurious".
        }

        U64 newDequeuePhys = DRV.DMA_EventRing->PhysAddr + ((U64)DRV.EventRingDequeueIndex * sizeof(xHCITRB));
        IR0->erdp = newDequeuePhys | (1u << 3);

        // clear IMAN IP
        IR0->iman = (1u << 1) | (1u << 0);

        // DUmmy read untuk mencegah CPU reordering
        volatile U32 dummy = IR0->iman;
        (void)dummy;

        DRV.SpuriousInterruptCount = 0; // Reset counter
        DRV.op_regs->usb_sts = (1u << 3); // ACK EINT
        volatile U32 dummy2 = DRV.op_regs->usb_sts;
        (void)dummy2;

        Arch::ASM::Mfence();
    }

    static VOID xHCI_HandleInterrupt(VAL32 Controller_ID){
        Serial::Printf( " Interrupt received from controller %u \n", (unsigned)Controller_ID);
        xHCIDriver &DRV = g_xhci_controllers[Controller_ID];


        /** 
        U32 Status = DRV.op_regs->usb_sts;
        if(!(Status & (1u << 3))){
            // Spurious interrupt: provide extra debug info to help diagnose IRQ races
            volatile xHCIInterrupterRegs *IR0 = &DRV.rt_regs->interrupter_regs[0];
            U32 iman = IR0->iman;
            Write(Level::LOG_INFO, " Spurious interrupt on controller %u\n", (unsigned)Controller_ID);
            Write(Level::LOG_INFO, " usb_sts=0x%08x IMAN=0x%08x ERDP=0x%016llx dequeue_idx=%u ring_size=%u\n",
                  (unsigned)Status,
                  (unsigned)iman,
                  (unsigned long long)IR0->erdp,
                  (unsigned)DRV.EventRingDequeueIndex,
                  (unsigned)DRV.EventRingSize);

            // If there are event TRBs present, dump the next couple for inspection
            if (DRV.VEventRing && DRV.EventRingSize) {
                U32 idx = DRV.EventRingDequeueIndex;
                for (int i = 0; i < 2; ++i) {
                    volatile xHCITRB *e = &DRV.VEventRing[idx];
                    Write(Level::LOG_INFO, " EventTRB[%u] param=0x%016llx status=0x%08x ctl=0x%08x\n",
                          (unsigned)idx,
                          (unsigned long long)e->parameter,
                          (unsigned)e->status,
                          (unsigned)e->control);
                    idx++;
                    if (idx == DRV.EventRingSize) idx = 0;
                }
            }

            // Increment spurious counter and, if no events are pending, attempt to clear
            // the interrupter pending bit to avoid IRQ storms.
            DRV.SpuriousInterruptCount++;

            // If IMAN.IP is set but Event Ring appears empty (next TRBs are zero), clear IP.
            // This is low-risk: it's an ack of an empty interrupt.
            if ( (iman & (1u << 1)) ) {
                bool ring_empty = true;
                if (DRV.VEventRing && DRV.EventRingSize) {
                    U32 idx2 = DRV.EventRingDequeueIndex;
                    // Check a few TRBs to see if non-zero
                    for (int i = 0; i < 4; ++i) {
                        volatile xHCITRB *et = &DRV.VEventRing[idx2];
                        if (et->control || et->status || et->parameter) { ring_empty = false; break; }
                        idx2++;
                        if (idx2 == DRV.EventRingSize) idx2 = 0;
                    }
                }

                if (ring_empty) {
                    // Re-arm the Event Ring Dequeue Pointer (ERDP) like the normal path does.
                    U64 newDequeuePhys = DRV.DMA_EventRing->PhysAddr + ((U64)DRV.EventRingDequeueIndex * sizeof(xHCITRB));
                    IR0->erdp = newDequeuePhys | (1u << 3);
                    asm volatile ("mfence" ::: "memory");

                    // Clear IP and ensure IE stays enabled (write 1 to IP, 1 to IE)
                    IR0->iman = (1u << 0) | (1u << 1);
                    U32 iman_after = IR0->iman;
                    Write(Level::LOG_INFO, " Re-armed ERDP and attempted clear IMAN.IP (before=0x%08x after=0x%08x)\n", (unsigned)iman, (unsigned)iman_after);

                    // If IMAN did not clear, try toggle IE fallback: disable IE then re-enable.
                    if (iman_after == iman) {
                        Write(Level::LOG_WARNING, " Controller %u - IMAN.IP did not clear, attempting IE toggle fallback (bus %u:%u:%u vector=0x%02x)\n",
                              (unsigned)Controller_ID, (unsigned)DRV.bus, (unsigned)DRV.dev, (unsigned)DRV.func, (unsigned)DRV.IntVector);
                        // Clear IE (write IMAN with IE=0, IP left alone)
                        IR0->iman = (0u << 0) | (0u << 1);
                        asm volatile ("mfence" ::: "memory");
                        // Re-enable IE and IP clear bit in case controller accepted it now
                        IR0->iman = (1u << 0) | (1u << 1);
                        asm volatile ("mfence" ::: "memory");
                        U32 iman_after2 = IR0->iman;
                        Write(Level::LOG_INFO, " IE toggle result IMAN before=0x%08x after_toggle=0x%08x\n", (unsigned)iman, (unsigned)iman_after2);
                    }
                }
            }

            // Warn if spurious interrupts accumulate
            const U32 SPURIOUS_WARN_THRESHOLD = 50;
            if (DRV.SpuriousInterruptCount && (DRV.SpuriousInterruptCount % SPURIOUS_WARN_THRESHOLD) == 0) {
                Write(Level::LOG_WARNING, " Controller %u - Spurious interrupts seen: %u\n", (unsigned)Controller_ID, (unsigned)DRV.SpuriousInterruptCount);
            }

            return;
        }
        ProcessPendingEvents(DRV, Controller_ID);
    } */

        ProcessPendingEvents(DRV, Controller_ID);
    }

    // ISR entry for controller 0 (accepts context parameter)
    void xHCI_InterruptHandler_C0(void *context) {
        (void)context;
        xHCI_HandleInterrupt(0);
    }
}
