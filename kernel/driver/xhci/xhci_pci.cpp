#define PRINTK_MODULE_NAME "XHCIPCI"
#include <rosval.h>
#include <logging.hpp>
#include <drivers/pci.hpp>
#include <mm.hpp>
#include "xhci.hpp"
#include "xhci_isr.hpp"
#include <string.hpp>
#include "../../filesys/iblockdevice.hpp"
#include "../../dev/devicemanager.hpp"
#include "../../filesys/devfs/devfs.hpp"
#include "../../filesys/vfs/vfs.hpp"

namespace xHCI {
    using namespace Printk;

    VOID RegisterController(U8 Bus, U8 Device, U8 Function, U8 MSICapOffset){
        if(g_xhci_controller_count >= XHCI_MAX_CONTROLLERS) {
            Write(Level::LOG_ERR, " Controller full\n");
        }

        // Handle potential 64-bit BAR for xHCI (commonly a 64-bit MMIO BAR)
        U32 Bar0LOW = PCI::ReadDword(Bus, Device, Function, 0x10);
        U32 Bar0HI  = PCI::ReadDword(Bus, Device, Function, 0x14);
        U64 BarType = (Bar0LOW & 0x6) >> 1; // 0=32-bit, 2=64-bit
        U64 RegPhys64 = (BarType == 0x2)
            ? (((U64)Bar0HI << 32) | (U64)(Bar0LOW & 0xFFFFFFF0))
            : (U64)(Bar0LOW & 0xFFFFFFF0);
        UPTR RegPhys = (UPTR)RegPhys64;
        // Align physical base down to page boundary before mapping
        UPTR RegPhysPage = RegPhys & ~(PAGE_SIZE - 1);

        // XHCI register space can span multiple pages (capability, op, runtime,
        // doorbells). Map a larger window (e.g. 16 pages = 64KiB) to cover
        // typical controllers instead of a single page which caused PFs.
        const SIZE_T MapPagesCount = 16;
        VOID *VirtAddr = PageAlloc::VirtualAllocPages(MapPagesCount);
        if(!VirtAddr){
            Write(Level::LOG_ERR, " Failed allocating virtual for XHCI\n");
            return;
        }

        // Avoid setting NX here (EFER.NXE may be clear during early boot).
        PFLAGS Flags = PAGE_PRESENT | PAGE_RW | PAGE_PCD;
        if(!PageAlloc::MapPages(KernelPML4, RegPhysPage, (UPTR)VirtAddr, MapPagesCount, Flags)) {
            Write(Level::LOG_ERR, " Failed mapping XHCI registers\n");
            return;
        }

        UPTR PageOffset = RegPhys & (PAGE_SIZE - 1);

        xHCIDriver &DRV = g_xhci_controllers[g_xhci_controller_count];
        DRV.bus = Bus;
        DRV.dev = Device;
        DRV.func = Function;
        DRV.regs_base = (volatile U8*)VirtAddr + PageOffset;
        DRV.Initialized = FALSE;
        DRV.IntVector = 0;

        if(MSICapOffset != 0){
            // Hook controller 0 to ISR for now
            U8 Vector = MSI::EnableMSI(Bus, Device, Function, MSICapOffset, xHCI_InterruptHandler_C0);
            if(Vector != 0){
                DRV.IntVector = Vector;
                Write(Level::LOG_DEBUG, " Enabled MSI on XHCI Controller %02X:%02X:%02X with vector 0x%02x\n",
                    (unsigned)Bus, (unsigned)Device, (unsigned)Function, (unsigned)Vector);
            } else {
                Write(Level::LOG_ERR, " Failed to enable MSI on XHCI Controller %02X:%02X:%02X\n",
                    (unsigned)Bus, (unsigned)Device, (unsigned)Function);
            }
        } else {
            // Try legacy INTx fallback
            U8 irq = PCI::EnableLegacyINTxForDevice(Bus, Device, Function, xHCI_InterruptHandler_C0);
            if (irq != 0) {
                DRV.IntVector = (U8)(0x20 + irq);
                Write(Level::LOG_DEBUG, " Enabled legacy INTx IRQ %u for XHCI Controller %02X:%02X:%02X (vector 0x%02x)\n",
                    (unsigned)irq, (unsigned)Bus, (unsigned)Device, (unsigned)Function, (unsigned)DRV.IntVector);
            } else {
                Write(Level::LOG_WARNING, " No MSI and legacy INTx unavailable for XHCI %02X:%02X:%02X\n",
                    (unsigned)Bus, (unsigned)Device, (unsigned)Function);
            }
        }

        g_xhci_controller_count++;

        // Create a small IBlockDevice wrapper so the controller can be
        // exposed via /dev as a device node (name format: "ud%c"). The
        // wrapper does not implement sector IO; it simply provides a name
        // so DevFS/DeviceManager can list the controller.
        class XHCIBlockDevice : public IBlockDevice {
        public:
            CHAR8 m_Name[8];
            int m_Index;
            XHCIBlockDevice(int idx){
                m_Index = idx;
                char letter = (char)('a' + (idx & 0x1F));
                m_Name[0] = 'u'; m_Name[1] = 'd'; m_Name[2] = letter; m_Name[3] = '\0';
            }
            virtual ~XHCIBlockDevice() {}
            virtual BOOL ReadSectors(U64 LBA, U32 Count, PageAlloc::DMAAlloc::DMABuffer **BufferOut) override { (void)LBA; (void)Count; (void)BufferOut; return FALSE; }
            virtual BOOL WriteSectors(U64 LBA, U32 Count, PageAlloc::DMAAlloc::DMABuffer *Buffer) override { (void)LBA; (void)Count; (void)Buffer; return FALSE; }
            virtual const CHAR8* GetDeviceName() override { return m_Name; }
        };

        // Instantiate and register the device
        int idx_for_name = g_xhci_controller_count - 1;
        XHCIBlockDevice *dev = new XHCIBlockDevice(idx_for_name);
        if(!dev) {
            Write(Level::LOG_ERR, " Failed allocating XHCI dev wrapper\n");
        } else {
            BOOL ok1 = DeviceManager::RegisterBlockDevice(dev);

            // Also try to register with DevFS mounted at /dev
            FileSystem* fs = nullptr; char rel[256];
            BOOL ok2 = FALSE;
            if (VFSManager::ResolvePath((const char*)"/dev", &fs, rel) && fs) {
                DevFS* devfs = (DevFS*)fs;
                ok2 = devfs->RegisterBlockDevice(dev, dev->GetDeviceName());
            }

            Write(Level::LOG_DEBUG, " XHCI: Registered controller node %s (devmgr=%d, devfs=%d)\n", dev->GetDeviceName(), ok1 ? 1 : 0, ok2 ? 1 : 0);
        }
        Write(Level::LOG_DEBUG, " Registered XHCI Controller %02X:%02X:%02X\n",
            (unsigned)Bus, (unsigned)Device, (unsigned)Function);

    }
}
