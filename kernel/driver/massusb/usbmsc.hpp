#pragma once

#include <rosval.h>
#include "../../filesys/iblockdevice.hpp"
#include "../xhci/xhci.hpp"

class USBMassStorage : public IBlockDevice{
    private:
        xHCI::xHCIDriver *m_Driver;
        U8 m_SlotID;
        U8 m_BulkInDCI;
        U8 m_BulkOutDCI;

        U32 m_BlockSize;
        U64 m_MaxLBA;

        U32 m_TagCounter;

        BOOL WaitForTransfer(U8 DCI);

        BOOL SendCBW(U8 LUN, U32 TransferLen, U8 Dir, U8 CmdLen, U8 *SCSI_CDB);
        BOOL GetCSW(U32 ExpectedTag);
    public:
        USBMassStorage(xHCI::xHCIDriver *driver, U8 slotID, U8 bulkInDCI, U8 bulkOutDCI);
        ~USBMassStorage();

        BOOL Initialize();

        virtual BOOL ReadSectors(U64 LBA, U32 Count, PageAlloc::DMAAlloc::DMABuffer **BufferOut) override;
        virtual BOOL WriteSectors(U64 LBA, U32 Count, PageAlloc::DMAAlloc::DMABuffer *Buffer) override;
        virtual const CHAR8* GetDeviceName() override { return "USBDisk"; } // Nanti bisa dibikin dinamis
};