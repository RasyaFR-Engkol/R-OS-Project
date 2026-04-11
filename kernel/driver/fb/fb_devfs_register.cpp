#include "../../filesys/devfs/devfs.hpp"
#include <logging.hpp>
#include <string.hpp>
#include "../../log/fbcon/fbcon.hpp"
#include "../../dev/devicemanager.hpp"
#include "bootinfo.h"
#include "framebuffer.hpp"
#include "rosval.h"

// Simple ICharDevice wrapper around FBConsole so we can register it with DevFS
class FBCharDevice : public ICharDevice {
public:
    FBCharDevice(const CHAR8* name){
        unsigned long long len = String::Strlen(name);
        unsigned long long tocpy = (len < sizeof(m_Name)-1) ? len : (sizeof(m_Name)-1);
        String::Memcpy(m_Name, name, tocpy);
        m_Name[tocpy] = '\0';
    }

    virtual ~FBCharDevice(){}

    virtual U32 Read(File *file, U8* buffer, U32 size) override {
        (void)buffer; (void)size; return 0; // no input from framebuffer
    }

    virtual U32 Write(File* file, U8* buffer, U32 size) override {
        // 1. Validasi
        if (!file || !buffer || size == 0) return 0;
        const FB::Info* KernelFB = FB::Get(); 
        if (!KernelFB) return 0;

        // 2. Logic Bound Check (Sama kayak sebelumnya)
        U64 VRamSize = (U64)KernelFB->pitch * KernelFB->height;
        if (file->CurrentPosition >= VRamSize) return 0;

        U32 BytesToWrite = size;
        if (file->CurrentPosition + size > VRamSize) {
            BytesToWrite = (U32)(VRamSize - file->CurrentPosition);
        }

        // 3. Pointer VRAM Target
        U8* VRAMBase = (U8*)KernelFB->base; 
        U8* TargetAddr = VRAMBase + file->CurrentPosition;

        // Buffer sudah di kernel space (berkat Sys_Write), jadi langsung copy aja.
        String::Memcpy(TargetAddr, buffer, BytesToWrite);

        // OPTIONAL: Flush Cache kalau pake PAGE_RW biasa (bukan PWT/WC)
        Arch::ASM::FlushCacheRange(TargetAddr, BytesToWrite); 

        // 4. Update Posisi
        file->CurrentPosition += BytesToWrite;

        return BytesToWrite;
    }

    virtual const CHAR8* GetDeviceName() override {
        return m_Name;
    }

    virtual INTN Ioctl(File* file, U32 command, U64 arg) override {
        const FB::Info* KernelFB = FB::Get();
        if(!KernelFB) return -ROS_ERR;

        Tasking::Task *Task = Tasking::GetCurrentTaskPtr();
        if(!Task) return -ROS_ERR;
        U64 *UserPML4 = HHDM_PhysToVirt(Task->CR3);

        switch(command){
            case FB_CMD_GET_INFO: {
                FB::UserFBInfo_T Info;
                Info.Width = KernelFB->width;
                Info.Bpp = KernelFB->bpp;
                Info.Height = KernelFB->height;
                Info.Pitch = KernelFB->pitch;
                Info.Size = (U64)Info.Pitch * Info.Height;

                if(!(PageAlloc::CopyToUser(UserPML4, (VOID*)arg, &Info, sizeof(Info)))){
                    Printk::Write(Printk::Level::LOG_DERR, "FB: IOCTL for FBCMDGETINFO part of copy to user failed.\n");
                    return -ROS_NOMEM;
                }

                return 0;
            }

            case FB_CMD_MAP_VRAM:{
                U64 CurrentPhysAddr = FB::GetPhysAddr(); // Fungsi baru tadi

                if (CurrentPhysAddr == 0) {
                    return -ROS_INVALSTATE;
                }
                U64 Size = (U64)KernelFB->pitch * KernelFB->height;
                U64 Pages = (Size + PAGE_SIZE - 1) / PAGE_SIZE + 1;

                // harusnya ada allocator buat user itu sendiri. tapi keknya belom ada
                // yaudah coba di hardcode dulu ke alamat yang lebih tinggi biar ga tabrakan
                // sama ELF Loader nya
                U64 UserVirtAddr = Task->MMapNextAddr;

                if(UserVirtAddr & 0xFFF){
                    UserVirtAddr = (UserVirtAddr + 0xFFF) & ~0xFFF; 
                }

                BOOL map_ok = PageAlloc::MapPages(
                    UserPML4, 
                    CurrentPhysAddr, 
                    UserVirtAddr, 
                    Pages, 
                    PAGE_PRESENT | PAGE_RW | PAGE_USER
                );

                if(!map_ok) return -ROS_NOMEM;

                Task->MMapNextAddr = UserVirtAddr + (Pages * PAGE_SIZE);

                if (!PageAlloc::CopyToUser(UserPML4, (void*)arg, &UserVirtAddr, sizeof(U64))) {
                    PageAlloc::UnMapPages(UserPML4, UserVirtAddr);
                    return -14; 
                }

                return 0;
            }

            case FB_CMD_DISABLE_CONSOLE: {
                Printk::Write(Printk::Level::LOG_INFO, "FBDev: Console disabled by User Request.\n");
                FBConsole::UpdateConsoleStatus(FALSE);
                return 0;
            }

            case FB_CMD_UPDATE: {
                const FB::Info* info = FB::Get();
                if(info) {
                    // PAKE YANG INI! Jangan FB::Flush() biasa.
                    // Kita asumsikan User Space udah nulis pixelnya ke VRAM (via mmap).
                    // Kita cuma butuh "Sinyal" ke QEMU.
                    FB::FlushHW(0, 0, info->width, info->height);
                }
                return 0;
            }

            case FB_PRESENT_SOME_AREA_TO_SCREEN: {
                FBRect *rect = (FBRect*)arg;
                if(rect) {
                    FB::FlushHW(rect->x, rect->y, rect->h, rect->w);
                }
                return 0;
            }
        }
        return -ROS_UNSUPPORTED;        
    }

    virtual short Poll(File *file, short events) override {
        return 0;
    }

private:
    CHAR8 m_Name[64];
};

namespace FBDriver{
    // Public helper: register a /dev/ttyfb0 entry into the provided DevFS.
    // If devfs is nullptr, this will try to register with DeviceManager only.
    BOOL RegisterFBToDevFS(DevFS* devfs){
        static FBCharDevice* inst = nullptr;
        if(inst) return TRUE; // already registered

        inst = new FBCharDevice((const CHAR8*)"fb0");
        if(!inst) return FALSE;

        BOOL ok1 = FALSE, ok2 = FALSE;
        if(devfs) ok1 = devfs->RegisterCharDevice(inst, (const CHAR8*)"fb0");
        ok2 = DeviceManager::RegisterCharDevice(inst);

        Printk::Write(Printk::Level::LOG_INFO, "FBDev: Registered fb0 (devfs=%d, devmgr=%d)\n", ok1 ? 1 : 0, ok2 ? 1 : 0);
        return TRUE;
    }
}