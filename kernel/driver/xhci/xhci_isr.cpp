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
#include <../firmware/acpi/acpi.hpp>
#include <../firmware/acpi/driver/timer/timer.hpp>

// Taruh di atas sebelum SetupAddressDevice dipanggil
static void SpinDelayMs(U64 ms) {
    // Asumsi lu udah nyimpen Freq TSC pas kalibrasi LAPIC kemarin
    U64 start = Arch::ASM::RdTSC();
    U64 ticks_to_wait = (ACPI::Timer::TSCFrequencyHz * ms) / 1000;
    U64 target = start + ticks_to_wait;
    
    while (Arch::ASM::RdTSC() < target) {
        Arch::ASM::PauseCPU(); // Biar CPU nggak kepanasan pas nunggu
    }
}

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

        // Ekstrak bit-bit penting biar gampang dibaca
        bool isConnected = (PortSC & 1);           // CCS: Current Connect Status
        bool isEnabled   = (PortSC & (1 << 1));    // PED: Port Enabled/Disabled
        bool csc         = (PortSC & (1 << 17));   // CSC: Connect Status Change
        bool prc         = (PortSC & (1 << 21));   // PRC: Port Reset Change

        // ====================================================
        // 1. EVENT: CONNECT STATUS CHANGE (Dicolok / Dicabut)
        // ====================================================
        if(csc) {
            PortReg->port_sc = (PortSC & ~0x00FE0000) | (1 << 17);

            if(isConnected) {
                if(isEnabled) {
                    Write(Level::LOG_NOTICE, " xHCI: Port %u Connected & Enabled (USB 3.0). Queueing Enable Slot...\n", (unsigned)PortID);
                    
                    DRV.PortStates[PortID - 1].State = xHCIDriver::PORT_STATE_ENABLE_SENT;
                    
                    U8 currentTail = DRV.EnableSlotQueueTail;
                    DRV.EnableSlotQueue[currentTail] = PortID;
                    DRV.EnableSlotQueueTail = currentTail + 1;

                    SendEnableSlotCommand(DRV);
                } else {
                    // KASUS B: MURNI USB 2.0 (Butuh Reset)
                    Write(Level::LOG_INFO, " xHCI: Port %u Connected (USB 2.0). Issuing Reset...\n", (unsigned)PortID);

                    DRV.PortStates[PortID - 1].ResetCount = 0;

                    SpinDelayMs(100);
                    
                    U32 ResetCMD = PortSC;
                    ResetCMD &= ~0x00FE0000; // Jangan clear status change lain
                    ResetCMD |= (1 << 4);    // Set PR (Port Reset)
                    ResetCMD &= ~(1 << 1);   // Pastikan PED tidak ketulis 1
                    PortReg->port_sc = ResetCMD;
                    
                    // Selesai. Nanti Controller akan nembak interrupt PRC.
                }
            } else {
                Write(Level::LOG_NOTICE, " xHCI: Port %u Disconnected. Cleaning up device...\n", (unsigned)PortID);

                U8 SlotID = DRV.PortStates[PortID - 1].SlotID;
                if(SlotID != 0){
                    Write(Level::LOG_INFO, " xHCI: Port %u - Removing device at Slot %u.\n", (unsigned)PortID, (unsigned)SlotID);
                    
                    U32 CMDIDX = DRV.CmdRingEnqueueIndex;
                    VOLATILE xHCITRB *CmdTRB = &DRV.VCmdRing[CMDIDX];

                    CmdTRB->parameter = 0;
                    CmdTRB->status = 0;
                    CmdTRB->control = (10u << 10) | // TRB_TYPE_DISABLE_SLOT
                                      (SlotID << 24) |
                                      (DRV.CmdRingCycleState ? 1u : 0u);

                    DRV.doorbell_regs[0] = 0; // Ring doorbell

                    DRV.CmdRingEnqueueIndex++;
                    if(DRV.CmdRingEnqueueIndex == DRV.CmdRingSize - 1){
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
        // 2. EVENT: PORT RESET CHANGE (Selesai Reset)
        // ====================================================
        // ====================================================
        // 2. EVENT: PORT RESET CHANGE (Selesai Reset)
        // ====================================================
        else if(prc){
            // Clear PRC
            PortReg->port_sc = (PortSC & ~0x00FE0000) | (1 << 21);
            
            U32 PostResetSC = PortReg->port_sc;
            int timeout = 100; // Maksimal nunggu 1 detik (100 * 10ms)

            // --- THE MAGIC REALTEK FIX ---
            // Cek bit 5-8 (Port Link State). Angka 7 artinya POLLING.
            while (((PostResetSC >> 5) & 0xF) == 7 && timeout > 0) {
                SpinDelayMs(200);
                
                PostResetSC = PortReg->port_sc; // Baca ulang status terbaru
                timeout--;
            }
            
            // Setelah lolos dari Polling, kasih napas dikit (Debounce tambahan)
            SpinDelayMs(200);
            PostResetSC = PortReg->port_sc;

            // BARU KITA CEK PED-NYA!
            bool nowConnected = (PostResetSC & 1);
            bool nowEnabled   = (PostResetSC & (1 << 1));

            if(nowConnected && nowEnabled) {
                Write(Level::LOG_NOTICE, " xHCI: Port %u Reset Complete (PRC). Queueing Enable Slot...\n", (unsigned)PortID);

                DRV.PortStates[PortID - 1].State = xHCIDriver::PORT_STATE_ENABLE_SENT;

                U8 currentTail = DRV.EnableSlotQueueTail;
                DRV.EnableSlotQueue[currentTail] = PortID;
                DRV.EnableSlotQueueTail = (currentTail + 1) % 256; 

                SendEnableSlotCommand(DRV);
            } else {
                if (DRV.PortStates[PortID - 1].ResetCount < 3) {
                    DRV.PortStates[PortID - 1].ResetCount++;
                    Write(Level::LOG_WARNING, " xHCI: Port %u PRC triggered but Link State stuck/failed! (SC=0x%08x). Retrying (%d/3)...\n", 
                          (unsigned)PortID, (unsigned)PostResetSC, DRV.PortStates[PortID - 1].ResetCount);

                    U32 ResetCMD = PortReg->port_sc;
                    ResetCMD &= ~0x00FE0000; 
                    ResetCMD |= (1 << 4);    
                    ResetCMD &= ~(1 << 1);   
                    PortReg->port_sc = ResetCMD;
                } else {
                    Write(Level::LOG_ERR, " xHCI: Port %u failed to enable after 3 retries. SC=0x%08x\n", (unsigned)PortID, (unsigned)PostResetSC);
                    DRV.PortStates[PortID - 1].State = xHCIDriver::PORT_STATE_EMPTY;
                }
            }
        }
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
            /*Serial::Printf(" xHCI: Controller %u - Event Type %u detected (param=0x%016llx status=0x%08x ctl=0x%08x)\n",
                (unsigned)Controller_ID, (unsigned)EventType,
                (unsigned long long)Event->parameter,
                (unsigned)Event->status,
                (unsigned)Event->control);*/

            // 33 = Command Completion Event, 34 = Port Status Change Event
            if(EventType == EVENT_TYPE_COMMAND_COMPLETION){
                U8  ccode   = (U8)(Event->status >> 24);
                U8  slotId  = (U8)(Event->control >> 24);
                U64 cmdTrbPhys = Event->parameter; // Pointer ke Command TRB asli

                U64 cleanPhys = cmdTrbPhys & ~0xFull; 
                
                // Kita harus intip Command TRB aslinya untuk tahu ini command apa
                volatile xHCITRB *OriginalCmd = (volatile xHCITRB*)HHDM_PhysToVirt((UPTR)cleanPhys);
                U32 CmdType = (OriginalCmd->control >> 10) & 0x3F; // Ambil TRB Type dari Command aslinya

                if(ccode == CC_SUCCESS){ // Success
                    
                    switch(CmdType) {
                        case TRB_TYPE_NOOP: // TRB_TYPE_NOOP
                            Write(Level::LOG_DEBUG, " xHCI: NOOP Command Completed.\n");
                        break;

                        case TRB_TYPE_ENABLE_SLOT: // TRB_TYPE_ENABLE_SLOT
                            {
                                U32 targetPort = 0;

                                // --- AMBIL DARI QUEUE (POP) ---
                                U8 currentHead = DRV.EnableSlotQueueHead;
                                
                                // Cek apakah Queue tidak kosong (Head belum menyusul Tail)
                                if (currentHead != DRV.EnableSlotQueueTail) {
                                    targetPort = DRV.EnableSlotQueue[currentHead]; // Ambil data
                                    DRV.EnableSlotQueueHead = currentHead + 1;     // Geser Head
                                }
                                // ------------------------------
                                
                                if(targetPort != 0) {
                                    U32 portSC = DRV.port_regs[targetPort - 1].port_sc;
                                    bool isConnected = (portSC & 1);
                                    bool isEnabled = (portSC & 2);
                                    
                                    if (!isConnected || !isEnabled) {
                                        Write(Level::LOG_WARNING, " xHCI: Slot %u assigned, but Port %u is already dead/disabled! Aborting.\n", slotId, targetPort);
                                        
                                        // TODO: Kirim Command "Disable Slot" buat ngebuang Slot ID ini biar gak mubazir
                                        
                                        DRV.PortStates[targetPort - 1].State = xHCIDriver::PORT_STATE_EMPTY;
                                        DRV.PortStates[targetPort - 1].SlotID = 0;
                                        break; // Jangan lanjut Address Device!
                                    }

                                    SpinDelayMs(50);
                                    DRV.PortStates[targetPort-1].SlotID = slotId; 
                                    SetupAddressDevice(DRV, slotId, targetPort);
                                } else {
                                    Write(Level::LOG_WARNING, " xHCI: Enable Slot completed but Queue is empty!\n");
                                }
                            }
                        break;

                        case TRB_TYPE_ADDRESS_DEVICE: // TRB_TYPE_ADDRESS_DEVICE
                            //Write(Level::LOG_INFO, " xHCI: Address Device Command Completed for Slot %u.\n", slotId);
                            // Baru disini aman panggil GetDescriptor
                            GetDeviceDescriptor(DRV, slotId);
                            break;

                        case TRB_TYPE_CONFIGURE_ENDPOINT: // TRB_TYPE_CONFIGURE_ENDPOINT
                        {
                            //Write(Level::LOG_INFO, " xHCI: Configure Endpoint Completed for Slot %u. Endpoint READY!\n", slotId);

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
                            U32 transferLen = 64;
                            if(transferLen == 0) transferLen = 8; // Fallback kalau 0
                            if(transferLen < 64) transferLen = 64; // SAFETY: Minta lebih banyak gak masalah (Short Packet), minta dikit bikin Babble.
                            if(DRV.Devs[slotId].IsMassStorage){
                                Write(Level::LOG_DEBUG, " xHCI: Mass Storage Device detected, mounting.\n");
                                
                                DRV.Devs[slotId].PendingMSCInit = TRUE;
                            } else if(DRV.Devs[slotId].IsMouse || DRV.Devs[slotId].IsKeyboard){
                                if (DRV.Devs[slotId].IsMouse) {
                                    transferLen = 8; // Atau lebih besar
                                    //Write(Level::LOG_DEBUG, " xHCI: Queueing first MOUSE Transfer for Slot %u\n", slotId);
                                } else {
                                    //Write(Level::LOG_DEBUG, " xHCI: Queueing first KEYBOARD Transfer for Slot %u\n", slotId);
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
                        case TRB_TYPE_EVALUATE_CONTEXT: //TRB_TYPE_EVALUATE_CONTEXT
                            {
                                //Write(Level::LOG_INFO, " xHCI: Evaluate Context Command Completed for Slot %u.\n", slotId);
                                // Biasanya ini cuma langkah validasi, kita gak perlu ngapa-ngapain di sini.
                            }
                            break;

                        case TRB_TYPE_DISABLE_SLOT: // TRB_TYPE_DISABLE_SLOT
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
            } else if (EventType == TRB_TYPE_TRANSFER_EVENT){ // Transfer Event
                U8 CCode = (U8)(Event->status >> 24);
                U8 SlotID = (U8)(Event->control >> 24);
                U8 dciSource = CalcDCISource(Event);
                auto &devState = DRV.Devs[SlotID];

                /*Serial::Printf( " [DEBUG] ISR Read Stage %d for Slot %u (Addr: 0x%016llx)\n", 
                    (int)devState.Stage, (unsigned)SlotID, (unsigned long long)&devState.Stage);*/

                if(CCode == CC_SUCCESS || CCode == CC_SHORT_PACKET){ // Success
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
                            //Write(Level::LOG_INFO, " xHCI: Get Device Descriptor DONE for Slot %u\n", SlotID);
                            
                            // Lanjut ke Next Step
                            devState.Stage = xHCIDriver::XHCIDeviceState::STAGE_SET_CONFIG_SENT;

                            SetDeviceConfiguration(DRV, SlotID, 1);
                            break;

                            case xHCIDriver::XHCIDeviceState::STAGE_SET_CONFIG_SENT:
                            {
                                //Write(Level::LOG_INFO, " xHCI: Set Configuration DONE for Slot %u\n", SlotID);
                                
                                // SEKARANG KITA BELOKIN: Jangan langsung GetDescriptor.
                                // Kita kirim Set Protocol dulu.
                                
                                devState.Stage = xHCIDriver::XHCIDeviceState::STAGE_SET_PROTOCOL_SENT; // Pindah Stage
                                SetBootProtocol(DRV, SlotID); // Kirim Command
                            }
                            break;

                        case xHCIDriver::XHCIDeviceState::STAGE_SET_PROTOCOL_SENT:
                            {
                                //Write(Level::LOG_INFO, " xHCI: Set Boot Protocol DONE for Slot %u. Now Reading Config...\n", SlotID);

                                // Protocol udah Boot Mode. Sekarang baru aman baca Descriptor.
                                
                                // Alokasi buffer uncached
                                PageAlloc::DMAAlloc::DMABuffer* descBuf = PageAlloc::DMAAlloc::AllocateDMABytes(256);
                                DRV.Devs[SlotID].LastEP0DestPhys = descBuf->PhysAddr; 

                                devState.Stage = xHCIDriver::XHCIDeviceState::STAGE_GET_CONFIG_DESC_SENT; // Pindah Stage
                                
                                GetDescriptor(DRV, SlotID, 2, 0, 256, descBuf->PhysAddr);
                            }
                            break;

                        case xHCIDriver::XHCIDeviceState::STAGE_GET_CONFIG_DESC_SENT:
                        {
                            // Cek apakah ini event duplikat (Status Stage)?
                            // Kita bisa cek apakah kita baru saja memproses ini.
                            // Tapi cara paling gampang: Ubah stage langsung setelah sukses.
                            
                            //Write(Level::LOG_INFO, " xHCI: Config Descriptor Received! Parsing...\n");
                            
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
                            //Serial::Printf(" [DEBUG] ISR Stage RUNNING for Slot %u DCI Source %u\n", (unsigned)SlotID, (unsigned)dciSource);
                            /**
                             * Berarti ini adalah HID Device (Mouse/Keyboard)
                             */

                            HandleIfHIDInput(devState, Event, CCode, dciSource);

                            // ===============================================
                            // KITA HARUS SELALU RE-QUEUE, GAK PEDULI CODE 1 ATAU 13
                            // ===============================================
                            U8 dci = devState.ActiveIntDCI ? devState.ActiveIntDCI : 3;
                            if (dciSource == dci) { 
                                // FIX: Gunakan logika size yang sama
                                U32 transferLen = devState.IntMaxPacketSize;
                                if(transferLen < 64) transferLen = 64; // Safety padding untuk mencegah Babble

                                QueueInterruptTransfer(DRV, SlotID, dci, devState.IntBufferPhys, transferLen);
                            }
                        }
                        break;
                                        
                        default:
                            Write(Level::LOG_WARNING, " xHCI: Unknown Transfer Event for Slot %u in Stage %d\n", SlotID, devState.Stage);
                            break;
                    }
                } 
                else if (CCode == CC_BABBLE_DETECTED) { // BABBLE ERROR
                    U32 Residual = (Event->status & 0x00FFFFFF);
                    Write(Level::LOG_ERR, " [BABBLE DEBUG] Slot %u. Residual: %u\n", SlotID, Residual);
                    
                    // --- TAMBAHAN DEBUGGING: DUMP ISI BUFFER ---
                    if(devState.IntBufferVirt) {
                        U8* b = devState.IntBufferVirt;
                        Write(Level::LOG_ERR, " [BABBLE DUMP] The data that actually arrived (Hex):\n");
                        // Print 16 byte pertama aja buat intip
                        Serial::Printf("   RAW: %02X %02X %02X %02X %02X %02X %02X %02X | %02X %02X %02X %02X ...\n",
                            b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                            b[8], b[9], b[10], b[11]);
                            
                        // Analisis Singkat
                        if(devState.IsMouse) {
                            Serial::Printf("   [MOUSE ANALYSIS] Buttons: 0x%02X, X: %d, Y: %d, Wheel?: %d\n",
                                b[0], (I8)b[1], (I8)b[2], (I8)b[3]);
                        }
                    }
                    
                    // --- FORCE RE-QUEUE SUPAYA GAK MATI ---
                    // Walaupun error, kita coba antrekan lagi dengan buffer LEBIH GEDE (misal 64)
                    // Supaya mouse tetep jalan dan kita bisa liat log selanjutnya.
                    U8 dci = devState.ActiveIntDCI ? devState.ActiveIntDCI : 3;
                    
                    // HARDCODE 64 byte buat testing!
                    Write(Level::LOG_WARNING, " [RECOVERY] Force Re-Queueing with 64 bytes...\n");
                    QueueInterruptTransfer(DRV, SlotID, dci, devState.IntBufferPhys, 64);
                }
            } else if (EventType == EVENT_TYPE_PORT_STATUS_CHANGE) {
                U8 PortID = (U8)((Event->parameter >> 24) & 0xFF);

                HandlePortStatusChange(DRV, PortID);
            } else {
                // Event lain yang belum kita handle khusus, cukup print aja dulu
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
        //Serial::Printf( " Interrupt received from controller %u \n", (unsigned)Controller_ID);
        xHCIDriver &DRV = g_xhci_controllers[Controller_ID];

        ProcessPendingEvents(DRV, Controller_ID);
    }

    // ISR entry for controller 0 (accepts context parameter)
    void xHCI_InterruptHandler_C0(void *context) {
        (void)context;
        xHCI_HandleInterrupt(0);
    }

    void xHCI_InterruptHandler_C0_TopHalf(void *context) {
        xHCIDriver &DRV = g_xhci_controllers[0];
        volatile xHCIInterrupterRegs *IR0 = &DRV.rt_regs->interrupter_regs[0];

        // 1. Cek apakah interrupt ini beneran dari xHCI? (EINT bit)
        if (!(DRV.op_regs->usb_sts & (1u << 3))) {
            return; // Spurious, abaikan.
        }

        // 2. ACK interupsi di hardware xHCI agar sinyal IRQ turun
        DRV.op_regs->usb_sts = (1u << 3); // Clear EINT
        IR0->iman = (1u << 0);            // Clear IP (Interrupt Pending)

        // 3. MASK (Matikan sementara) interupsi xHCI ini supaya nggak nembak lagi 
        // sebelum Bottom Half kita selesai mikir.
        IR0->iman &= ~(1u << 1);          // Disable IE (Interrupt Enable)

        // 4. JADWALKAN BOTTOM HALF!
        Scheduler::Signal(&g_xhci_controllers[0].InterruptSignal);
    }

    void xHCI_Worker_Thread(void* arg) {
        U32 Controller_ID = (U32)(UPTR)arg;
        xHCIDriver &DRV = g_xhci_controllers[Controller_ID];
        volatile xHCIInterrupterRegs *IR0 = &DRV.rt_regs->interrupter_regs[0];

        while (true) {
            // 1. Tidur sampai Top Half membangunkan kita
            Scheduler::WaitForSignal(&DRV.InterruptSignal);

            // 2. Oke kita bangun! Sekarang proses event ring xHCI dengan tenang.
            // Di sini kamu BEBAS pakai SpinDelayMs, AllocateDMABytes, Printk, dll!
            ProcessPendingEvents(DRV, Controller_ID);

            // 3. Kerjaan selesai. NYALAKAN LAGI interupsi xHCI agar hardware 
            // bisa ngasih tahu kalau ada event baru.
            IR0->iman |= (1u << 1); // Enable IE
        }
    }
}
