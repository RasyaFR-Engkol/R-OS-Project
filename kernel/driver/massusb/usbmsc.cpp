#include <rosval.h>
#include "rossys.hpp"
#include "rostime.hpp"
#include "usb_defs.hpp"
#include "usbmsc.hpp"
#include <string.hpp>
#define PRINTK_MODULE_NAME "USBMSC"
#include <logging.hpp>

static U16 Swap16(U16 v) { return (v << 8) | (v >> 8); }
static U32 Swap32(U32 v) { return (v << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24); }

USBMassStorage::USBMassStorage(xHCI::xHCIDriver *driver, U8 slotID, U8 bulkInDCI, U8 bulkOutDCI){
    m_Driver = driver;
    m_SlotID = slotID;
    m_BulkInDCI = bulkInDCI;
    m_BulkOutDCI = bulkOutDCI;

    m_BlockSize = 512; // Default block size
    m_MaxLBA = 0;

    m_TagCounter = 0;

    // Log debug (Opsional)
    Printk::Write(Printk::Level::LOG_DEBUG, "USB MSC: Object Created for Slot %u (In:%u Out:%u)\n", slotID, bulkInDCI, bulkOutDCI);
}

USBMassStorage::~USBMassStorage() {
    // Di sini tempat bersih-bersih kalau ada buffer permanen yang dialokasikan di constructor.
    // Karena implementasi kita buffer-nya alokasi on-demand (per transfer), 
    // destructor ini cukup kosong atau log aja.
    Printk::Write(Printk::Level::LOG_INFO, "USB MSC: Device Detached/Destroyed.\n");
}

BOOL USBMassStorage::SendCBW(U8 LUN, U32 TransferLen, U8 Dir, U8 CmdLen, U8 *SCSI_CDB){
    auto *cbwBuff = PageAlloc::DMAAlloc::AllocateDMABytes(31);
    if(!cbwBuff) return FALSE;

    CommandBlockWrapper *CBW = (CommandBlockWrapper*)cbwBuff->VirtAddr;

    String::Memset((U8*)CBW, 0, 31);

    CBW->dCBWSignature = 0x43425355; // 'USBC'
    CBW->dCBWTag = ++m_TagCounter;
    CBW->dCBWDataTransferLength = TransferLen;

    CBW->bmCBWFlags = Dir ? 0x80 : 0x00; // bit7: 1=Data-In, 0=Data-Out
    CBW->bCBWLUN = LUN;
    CBW->bCBWCBLength = CmdLen;

    String::Memcpy(CBW->CBWCB, SCSI_CDB, CmdLen);

    m_Driver->Devs[m_SlotID].TransferComplete = FALSE;

    xHCI::QueueBulkTransfer(*m_Driver, m_SlotID, m_BulkOutDCI, cbwBuff->PhysAddr, 31);

    Serial::Printf("USB MSC: Sent CBW Doorbell (Tag: 0x%08x, Len: %u, Dir: %s)\n", 
        (unsigned)CBW->dCBWTag, (unsigned)TransferLen, Dir ? "IN" : "OUT");

    Arch::ASM::Mfence();

    m_Driver->doorbell_regs[m_SlotID] = m_BulkOutDCI;

    if(!WaitForTransfer(m_BulkOutDCI)){
        PageAlloc::DMAAlloc::FreeDMABuffer(cbwBuff);
        return FALSE;
    }

    PageAlloc::DMAAlloc::FreeDMABuffer(cbwBuff);
    return TRUE;
}

BOOL USBMassStorage::GetCSW(U32 ExpectedTag){
    auto *cswBuff = PageAlloc::DMAAlloc::AllocateDMABytes(13);
    if(!cswBuff) return FALSE;

    m_Driver->Devs[m_SlotID].TransferComplete = FALSE;

    xHCI::QueueBulkTransfer(*m_Driver, m_SlotID, m_BulkInDCI, cswBuff->PhysAddr, 13);

    Serial::Printf("USB MSC: Sent CSW Doorbell\n");
    Arch::ASM::Mfence();
    m_Driver->doorbell_regs[m_SlotID] = m_BulkOutDCI;

    if(!WaitForTransfer(m_BulkInDCI)){
        PageAlloc::DMAAlloc::FreeDMABuffer(cswBuff);
        return FALSE;
    }

    CommandStatusWrapper *csw = (CommandStatusWrapper*)cswBuff->VirtAddr;

    BOOL Success = FALSE;
    // Validasi Signature "USBS" dan Tag
    if (csw->dCSWSignature == 0x53425355 && csw->dCSWTag == ExpectedTag) {
        if (csw->bCSWStatus == 0) { // 0 = Command Passed
            Success = TRUE;
        } else {
            Printk::Write(Printk::Level::LOG_ERR, "USB MSC: CSW Error Status 0x%x\n", csw->bCSWStatus);
        }
    }

    PageAlloc::DMAAlloc::FreeDMABuffer(cswBuff);
    return Success;
}

BOOL USBMassStorage::ReadSectors(U64 LBA, U32 Count, PageAlloc::DMAAlloc::DMABuffer **BufferOut) {
    if (Count == 0) return FALSE;

    // 1. Hitung ukuran Byte
    U32 ByteLen = Count * m_BlockSize;

    // 2. Siapkan Buffer Data (Host terima data disini)
    *BufferOut = PageAlloc::DMAAlloc::AllocateDMABytes(ByteLen);
    if (!*BufferOut) return FALSE;

    // 3. Siapkan SCSI CDB (Command Descriptor Block) untuk READ(10)
    // Structure: [Opcode] [Flags] [LBA (Big Endian)] [Group] [Len (Big Endian)] [Control]
    U8 cdb[10];
    String::Memset(cdb, 0, 10);

    cdb[0] = 0x28; // Operation Code: READ (10)
    
    // LBA (Big Endian, 4 bytes)
    cdb[2] = (LBA >> 24) & 0xFF;
    cdb[3] = (LBA >> 16) & 0xFF;
    cdb[4] = (LBA >> 8) & 0xFF;
    cdb[5] = (LBA & 0xFF);

    // Transfer Length (Sectors) (Big Endian, 2 bytes)
    cdb[7] = (Count >> 8) & 0xFF;
    cdb[8] = (Count & 0xFF);

    // =============================
    // PHASE 1: COMMAND (CBW)
    // =============================
    // Dir = 1 (Data IN / Device-to-Host)
    if (!SendCBW(0, ByteLen, 1, 10, cdb)) {
        return FALSE;
    }

    // =============================
    // PHASE 2: DATA TRANSFER
    // =============================
    // Kita baca Data dari Bulk IN

    m_Driver->Devs[m_SlotID].TransferComplete = FALSE;

    xHCI::QueueBulkTransfer(*m_Driver, m_SlotID, m_BulkInDCI, (*BufferOut)->PhysAddr, ByteLen);
    
    if (!WaitForTransfer(m_BulkInDCI)) {
        return FALSE;
    }

    // =============================
    // PHASE 3: STATUS (CSW)
    // =============================
    if (!GetCSW(m_TagCounter)) {
        // Kalau CSW gagal, berarti command gagal.
        // Dalam implementasi nyata, mungkin perlu "Clear Feature HALT" endpoint disini.
        return FALSE;
    }

    return TRUE;
}

BOOL USBMassStorage::WaitForTransfer(U8 DCI) {
    auto &dev = m_Driver->Devs[m_SlotID];

    // Timeout counter
    U64 timeout = 1000000000; 
    while (!dev.TransferComplete && timeout > 0) {
         Arch::ASM::PauseCPU();
         timeout--;
    }

    if (timeout == 0) {
        Printk::Write(Printk::Level::LOG_ERR, "USB MSC: Transfer Timeout!\n");
        return FALSE;
    }
    return TRUE;
}

BOOL USBMassStorage::WriteSectors(U64 LBA, U32 Count, PageAlloc::DMAAlloc::DMABuffer *Buffer) {
    if (Count == 0 || !Buffer) return FALSE;

    // 1. Hitung ukuran Byte
    U32 ByteLen = Count * m_BlockSize; // BlockSize biasanya 512

    // 2. Siapkan SCSI CDB untuk WRITE(10)
    U8 cdb[10];
    String::Memset(cdb, 0, 10);

    cdb[0] = 0x2A; // Opcode: WRITE (10)
    
    // LBA (Big Endian)
    cdb[2] = (LBA >> 24) & 0xFF;
    cdb[3] = (LBA >> 16) & 0xFF;
    cdb[4] = (LBA >> 8) & 0xFF;
    cdb[5] = (LBA & 0xFF);

    // Transfer Length (Big Endian)
    cdb[7] = (Count >> 8) & 0xFF;
    cdb[8] = (Count & 0xFF);

    // =============================
    // PHASE 1: COMMAND (CBW)
    // =============================
    // Dir = 0 (Data OUT / Host-to-Device)
    if (!SendCBW(0, ByteLen, 0, 10, cdb)) {
        return FALSE;
    }

    // =============================
    // PHASE 2: DATA TRANSFER (BULK OUT)
    // =============================
    // Kirim data Buffer kita ke Endpoint Bulk OUT

    m_Driver->Devs[m_SlotID].TransferComplete = FALSE;

    xHCI::QueueBulkTransfer(*m_Driver, m_SlotID, m_BulkOutDCI, Buffer->PhysAddr, ByteLen);
    
    if (!WaitForTransfer(m_BulkOutDCI)) {
        return FALSE;
    }

    // =============================
    // PHASE 3: STATUS (CSW)
    // =============================
    if (!GetCSW(m_TagCounter)) {
        return FALSE;
    }

    return TRUE;
}

// usbmsc.cpp

// Helper function untuk convert Big Endian (Network Byte Order) ke Little Endian (Host)
static U32 Swap32(U8 *data) {
    return (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | (data[3]);
}

BOOL USBMassStorage::Initialize() {
    Printk::Write(Printk::Level::LOG_INFO, "USB MSC: Initializing...\n");

    // ==========================================
    // 1. LOOP TEST UNIT READY (SCSI Opcode 0x00)
    // ==========================================
    // Flashdisk kadang masih "Not Ready" pas baru dicolok. Kita retry beberapa kali.
    BOOL isReady = FALSE;
    for (int i = 0; i < 10; i++) {
        U8 cdb[10]; // Command Descriptor Block
        String::Memset(cdb, 0, 10);
        cdb[0] = 0x00; // Opcode: TEST UNIT READY

        // Kirim via CBW (Data Length = 0, Dir = 0, CDB Len = 6)
        // Group 0 commands (kayak Test Unit Ready) biasanya CDB-nya 6 bytes.
        // Tapi kita pass buffer 10 byte aman-aman aja karena bCBWCBLength kita set 6.
        if (SendCBW(0, 0, 0, 6, cdb)) {
            // Langsung ambil CSW (Gak ada fase Data Transfer)
            if (GetCSW(m_TagCounter)) {
                isReady = TRUE;
                break;
            }
        }
        // Belum ready, tunggu sebentar sebelum retry
    }

    if (!isReady) {
        Printk::Write(Printk::Level::LOG_ERR, "USB MSC: Device failed to become Ready (Time out).\n");
        return FALSE;
    }

    // ==========================================
    // 2. READ CAPACITY (10) (SCSI Opcode 0x25)
    // ==========================================
    // Kita butuh buffer 8 byte buat nerima respon:
    // Byte 0-3: Last Logical Block Address (Max LBA)
    // Byte 4-7: Block Length (biasanya 512)
    
    auto *capBuf = PageAlloc::DMAAlloc::AllocateDMABytes(8);
    if (!capBuf) return FALSE;

    U8 cdb[10];
    String::Memset(cdb, 0, 10);
    cdb[0] = 0x25; // Opcode: READ CAPACITY (10)

    // Kirim CBW (Data Length = 8, Dir = 1/IN, CDB Len = 10)
    if (!SendCBW(0, 8, 1, 10, cdb)) {
        PageAlloc::DMAAlloc::FreeDMABuffer(capBuf);
        return FALSE;
    }

    // Terima Data (Bulk IN)
    xHCI::QueueBulkTransfer(*m_Driver, m_SlotID, m_BulkInDCI, capBuf->PhysAddr, 8);

    Serial::Printf("USB MSC: Sent READ CAPACITY Doorbell\n");
    Arch::ASM::Mfence();

    m_Driver->doorbell_regs[m_SlotID] = m_BulkOutDCI;
    
    if (!WaitForTransfer(m_BulkInDCI)) {
        Printk::Write(Printk::Level::LOG_ERR, "USB MSC: Read Capacity Data Transfer Timeout.\n");
        PageAlloc::DMAAlloc::FreeDMABuffer(capBuf);
        return FALSE;
    }

    // Ambil Status (CSW)
    if (!GetCSW(m_TagCounter)) {
        Printk::Write(Printk::Level::LOG_ERR, "USB MSC: Read Capacity CSW Failed.\n");
        PageAlloc::DMAAlloc::FreeDMABuffer(capBuf);
        return FALSE;
    }

    // ==========================================
    // 3. PARSE DATA (BIG ENDIAN)
    // ==========================================
    U8 *data = (U8*)capBuf->VirtAddr;
    
    U32 lastLba = Swap32(&data[0]);    // Max LBA
    U32 blkSize = Swap32(&data[4]);    // Sector Size

    m_MaxLBA = lastLba;
    m_BlockSize = blkSize;

    // Hitung ukuran dalam MB untuk display
    U64 SizeMB = ((U64)(m_MaxLBA + 1) * m_BlockSize) / (1024 * 1024);

    Printk::Write(Printk::Level::LOG_NOTICE, "USB MSC: Device Ready! Size: %llu MB (LBA: %u, BlockSize: %u)\n", 
        SizeMB, m_MaxLBA, m_BlockSize);

    PageAlloc::DMAAlloc::FreeDMABuffer(capBuf);
    return TRUE;
}