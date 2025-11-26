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
            if((PortSC & 1) && !(PortSC & 2)) {
                Write(Level::LOG_NOTICE, " xHCI: Port %u Connected. Initiating Reset...\n", (unsigned)PortID);
                
                U32 ResetCMD = PortSC;
                ResetCMD &= ~0x00FE0000; // Mask status bits
                ResetCMD |= (1 << 4);    // Set PR (Port Reset)
                ResetCMD &= ~(1 << 1);   // Clear PED (biar reset jalan)
                
                PortReg->port_sc = ResetCMD;
                
                // KITA KELUAR DISINI. Jangan ditungguin pake while loop!
                // Biarkan hardware kerja. Nanti kalau reset kelar,
                // dia bakal kirim interrupt lagi dengan bit PRC (21) nyala.
                return; 
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
    }

    static VOID ProcessPendingEvents(xHCIDriver &DRV, U32 Controller_ID){
        volatile xHCIInterrupterRegs *IR0 = &DRV.rt_regs->interrupter_regs[0];
        while(TRUE){
            U32 index = DRV.EventRingDequeueIndex;
            volatile xHCITRB *Event = &DRV.VEventRing[index];

            asm volatile("clflush (%0)" :: "r"(Event) : "memory");
            asm volatile("mfence" ::: "memory"); // Tunggu memori sinkron

            U32 control = Event->control;

            if ((control & 1u) != (DRV.EventRingCycleState ? 1u : 0u)) {
                break;
            }

            U8 EventType = (U8)((control >> 10) & 0x3Fu);
            Printk::Write(Printk::Level::LOG_DEBUG, " xHCI: Controller %u - Event Type %u detected (param=0x%016llx status=0x%08x ctl=0x%08x)\n",
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

                                // --- LOGIC TAMBAHAN: TRIGGER PERTAMA ---
                                
                                // 1. Ambil DCI Interrupt yang tadi kita simpan pas parsing Descriptor
                                U8 targetDCI = DRV.Devs[slotId].ActiveIntDCI;
                                if(targetDCI == 0) {
                                    // Fallback kalau parsing gagal/belum diimplementasi: Asumsi EP 1 IN
                                    targetDCI = 3; 
                                }

                                // 2. Siapkan Buffer (Misal 8 byte cukup buat Mouse/Keyboard Boot Protocol)
                                // Simpan buffer ini di struct biar bisa dibaca nanti
                                if(DRV.Devs[slotId].IntBufferVirt == nullptr) {
                                    PageAlloc::DMAAlloc::DMABuffer* intBuf = PageAlloc::DMAAlloc::AllocateDMABytes(8);
                                    DRV.Devs[slotId].IntBufferPhys = intBuf->PhysAddr;
                                    DRV.Devs[slotId].IntBufferVirt = (U8*)intBuf->VirtAddr;
                                }

                                // 3. Masukkan "Pancingan" (Transfer TRB) ke Ring Endpoint
                                Write(Level::LOG_DEBUG, " xHCI: Queueing first Interrupt Transfer for Slot %u DCI %u\n", slotId, targetDCI);
                                
                                QueueInterruptTransfer(DRV, slotId, targetDCI, DRV.Devs[slotId].IntBufferPhys, 8);

                                // 4. Update State jadi RUNNING
                                DRV.Devs[slotId].Stage = xHCIDriver::XHCIDeviceState::STAGE_RUNNING;
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
                                QueueInterruptTransfer(DRV, slotId, targetDCI, intBuf->PhysAddr, 8);

                                // 4. Update Doorbell! (PENTING)
                                // Tanpa ini, xHCI gak akan sadar ada TRB baru di Endpoint Ring
                                DRV.doorbell_regs[slotId] = targetDCI; 

                                // 5. Update State
                                DRV.Devs[slotId].Stage = xHCIDriver::XHCIDeviceState::STAGE_RUNNING;
                                
                                Write(Level::LOG_NOTICE, " xHCI: Slot %u is now RUNNING! Waiting for mouse input...\n", slotId);
                            }
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
                
                auto &devState = DRV.Devs[SlotID];

                if(CCode == 1 || CCode == 13){ // Success
                    
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
                        BOOL found = FALSE;

                        while (offset < totalLen) {
                            U8 len = buffer[offset];
                            U8 type = buffer[offset + 1];
                            if (len == 0) break;

                            if (type == 5) { // ENDPOINT
                                U8 addr = buffer[offset + 2];
                                U8 attr = buffer[offset + 3];
                                U16 pkt = buffer[offset + 4] | (buffer[offset + 5] << 8);
                                U8 interval = buffer[offset + 6];

                                if ((attr & 0x3) == 3 && (addr & 0x80)) {
                                    
                                    // === FIX 1: SAFETY MARGIN UNTUK MOUSE GAMING ===
                                    // Jangan set pas-pasan 8. Mouse gaming suka lebay ngirim data.
                                    // Set ke 64 (Max Full Speed) biar aman dari BABBLE ERROR.
                                    // xHCI gak peduli kalau device cuma pake 4 dari 64. Aman.
                                    if (pkt < 64) {
                                        Write(Level::LOG_WARNING, "   [FIX] Bumping MPS from %u to 64 to prevent Babble on gaming mouse.\n", pkt);
                                        pkt = 64; 
                                    }

                                    Write(Level::LOG_NOTICE, "   >>> FOUND MOUSE/KBD ENDPOINT! DCI=%u (Addr=0x%x) MPS=%u\n", ((addr&0xF)*2)+1, addr, pkt);
                                    
                                    DRV.Devs[SlotID].ActiveIntDCI = ((addr & 0xF) * 2) + 1;
                                    
                                    ConfigureEndpoint(DRV, SlotID, addr, 7, pkt, interval);
                                    found = TRUE;
                                    break; 
                                }
                            }
                            offset += len;
                        }

                        if (found) {
                            // === FIX 2: MENCEGAH DOUBLE CONFIG ===
                            // Langsung pindah state supaya event berikutnya (Status Stage)
                            // tidak memicu parsing ulang.
                            DRV.Devs[SlotID].Stage = xHCIDriver::XHCIDeviceState::STAGE_ENDPOINT_CONFIG_SENT;
                        } else {
                            // Kalau gak ketemu, jangan stuck looping
                            Write(Level::LOG_ERR, "   No Interrupt Endpoint found. Parking device.\n");
                            DRV.Devs[SlotID].Stage = xHCIDriver::XHCIDeviceState::STAGE_CONFIGURED; 
                        }
                    }
                    break;
                            

                        case xHCIDriver::XHCIDeviceState::STAGE_ENDPOINT_CONFIG_SENT: 
                            // INI TRICKY: Configure Endpoint itu menghasilkan COMMAND COMPLETION EVENT (Type 33), BUKAN Transfer Event (Type 32).
                            // Jadi logic "Configure Selesai" harusnya ada di blok (EventType == 33).
                            // Tapi gapapa, kita pindahin logic-nya nanti.
                            break;

                        
                        case xHCIDriver::XHCIDeviceState::STAGE_CONFIGURED:
                            // Ini mungkin interrupt dari transfer data mouse/keyboard biasa
                            // Handle data endpoint disini nanti
                            break;

                        case xHCIDriver::XHCIDeviceState::STAGE_RUNNING:
                        {
                            U8* data = devState.IntBufferVirt;
                            
                            // Hitung data beneran yang masuk
                            // Transfer Length (Residual) ada di 24 bit bawah status
                            U32 residual = (Event->status & 0x00FFFFFF);
                            U32 lengthRequested = 8; // Sesuai buffer yang kita alokasi
                            U32 actualLength = lengthRequested - residual;

                            Write(Level::LOG_NOTICE, " [HID INPUT Slot %u] Len=%u Data: %02x %02x %02x %02x\n", 
                                SlotID, actualLength, data[0], data[1], data[2], data[3]);
                            
                            // === LOGIC MOUSE ===
                            // Byte 0: Bitmap Button (Bit 0=Left, 1=Right, 2=Middle)
                            // Byte 1: X Offset (Signed char)
                            // Byte 2: Y Offset (Signed char)
                            if (actualLength >= 3) {
                                I8 x_rel = (I8)data[1];
                                I8 y_rel = (I8)data[2];
                                if(x_rel != 0 || y_rel != 0) {
                                    Write(Level::LOG_INFO, "    Mouse Gerak: X=%d Y=%d\n", x_rel, y_rel);
                                }
                            }

                            // ===============================================
                            // INI YANG BIKIN MOUSE LU MATI SEBELUMNYA
                            // KITA HARUS SELALU RE-QUEUE, GAK PEDULI CODE 1 ATAU 13
                            // ===============================================
                            U8 dci = devState.ActiveIntDCI ? devState.ActiveIntDCI : 3;
                            QueueInterruptTransfer(DRV, SlotID, dci, devState.IntBufferPhys, 8);
                        }
                        break;
                                        
                        default:
                            Write(Level::LOG_WARNING, " xHCI: Unknown Transfer Event for Slot %u in Stage %d\n", SlotID, devState.Stage);
                            break;
                    }

                } 
                else {
                    // Nah, kalau ini baru error beneran (selain 1 dan 13)
                    Printk::Write(Printk::Level::LOG_ERR, " Transfer Failed Code %d Slot %u\n", CCode, SlotID);
                    // Handle error beneran (Stall, Babble, dll)
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

            index++;
            if(index == DRV.EventRingSize){
                index = 0;
                DRV.EventRingCycleState = !DRV.EventRingCycleState;
            }

            DRV.EventRingDequeueIndex = index;
        }

        Printk::Write(Printk::Level::LOG_DEBUG, " xHCI: Controller %u - No more pending events. Spurious Interrupt Handled\n", (unsigned)Controller_ID);

        // Ack Event Interrupt (EINT)
        DRV.op_regs->usb_sts = (1u << 3);
            U64 newDequeuePhys = DRV.DMA_EventRing->PhysAddr + ((U64)DRV.EventRingDequeueIndex * sizeof(xHCITRB));
            IR0->erdp = newDequeuePhys | (1u << 3);
        // Clear Interrupter Pending (write-1-to-clear) and ensure IE stays enabled.
        // Use an assignment to write the proper bits (clear IP by writing 1, and set IE).
        // Some implementations expect writing 1 to IP clears it; OR-ing the read value
        // is incorrect because it may not write the required '1'. So write both bits.
        IR0->iman = (1u << 1) | (1u << 0);
        asm volatile("mfence" ::: "memory");
    }

    static VOID xHCI_HandleInterrupt(VAL32 Controller_ID){
        Write(Level::LOG_INFO, " Interrupt received from controller %u \n", (unsigned)Controller_ID);
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
