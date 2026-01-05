#pragma once

#include "../../filesys/iblockdevice.hpp"
#include "ahci.hpp"
#include "ahci_internal.hpp"
#include "ahci_regs.hpp"
#include "string.hpp"
#include <filesystem/filesystem.hpp>
#include "../../dev/devicemanager.hpp"
#include "../../filesys/devfs/devfs.hpp"
#include "../../filesys/vfs/vfs.hpp"

class AHCIBlockDevice : public IBlockDevice {
    private:
        AHCI::AHCIDriver *m_Driver;
        U8 m_PortNumber;
        CHAR8 m_DeviceName[32];

    public:
        AHCIBlockDevice(AHCI::AHCIDriver *driver, U8 PortNumber) {
            m_Driver = driver;
            m_PortNumber = PortNumber;
            char letter = 'a' + PortNumber;
            m_DeviceName[0] = 's';
            m_DeviceName[1] = 'd';
            m_DeviceName[2] = letter;
            m_DeviceName[3] = '\0';

            // Try to register with the /dev DevFS instance if mounted so the
            // device appears as a node (e.g. "/dev/sda"). DeviceManager
            // registration is performed by the AHCI controller code that
            // instantiates this object, so we only register with DevFS here
            // to mirror the FB driver's behavior.
            FileSystem* fs = nullptr; char rel[256];
            if (VFSManager::ResolvePath((const char*)"/dev", &fs, rel) && fs) {
                DevFS* devfs = (DevFS*)fs;
                devfs->RegisterBlockDevice(this, m_DeviceName);
            }
        }

        virtual ~AHCIBlockDevice() {}

        virtual BOOL ReadSectors(U64 LBA, U32 Count, PageAlloc::DMAAlloc::DMABuffer **BufferOut) override {
            // Panggil fungsi AHCI yang sudah ada
            return AHCI::ReadSectors(*m_Driver, m_PortNumber, LBA, Count, BufferOut);
        }

        virtual BOOL WriteSectors(U64 LBA, U32 Count, PageAlloc::DMAAlloc::DMABuffer *Buffer) override {
            // Panggil fungsi AHCI yang sudah ada
            return AHCI::WriteSectors(*m_Driver, m_PortNumber, LBA, Count, Buffer);
        }

        virtual const CHAR8* GetDeviceName() override {
            return m_DeviceName;
        }
};