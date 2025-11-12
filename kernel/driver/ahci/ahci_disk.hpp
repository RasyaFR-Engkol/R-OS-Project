#pragma once

#include "../../filesys/iblockdevice.hpp"
#include "ahci.hpp"
#include "ahci_internal.hpp"
#include "ahci_regs.hpp"
#include "string.hpp"

class AHCIBlockDevice : public IBlockDevice {
    private:
        AHCI::AHCIDriver m_Driver;
        U8 m_PortNumber;
        CHAR8 m_DeviceName[32];

    public:
        AHCIBlockDevice(AHCI::AHCIDriver driver, U8 PortNumber) {
            m_Driver = driver;
            m_PortNumber = PortNumber;
            String::Strcpy(m_DeviceName, "ahci");
        }

        virtual ~AHCIBlockDevice() {}

        virtual BOOL ReadSectors(U64 LBA, U32 Count, PageAlloc::DMAAlloc::DMABuffer **BufferOut) override {
            // Panggil fungsi AHCI yang sudah ada
            return AHCI::ReadSectors(m_Driver, m_PortNumber, LBA, Count, BufferOut);
        }

        virtual BOOL WriteSectors(U64 LBA, U32 Count, PageAlloc::DMAAlloc::DMABuffer *Buffer) override {
            // Panggil fungsi AHCI yang sudah ada
            return AHCI::WriteSectors(m_Driver, m_PortNumber, LBA, Count, Buffer);
        }

        virtual const char* GetDeviceName() override {
            return m_DeviceName;
        }
};