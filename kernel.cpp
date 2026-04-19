// Minimal kernel entry implementation that prints to serial (COM1)
// This helps verify that we've actually jumped into 64-bit kernel_main.
#include "kernel/dev/devicemanager.hpp"
#include "kernel/filesys/filesystem.hpp"
#include "kernel/filesys/partition.hpp"
#define PRINTK_MODULE_NAME "Kernel"
#include <stdint.h>
#include <serial.hpp>
#include <rosval.h>
#include <bootinfo.h>
#include "firmware/acpi/acpi.hpp"
#include "firmware/acpi/driver/timer/timer.hpp"
#include "framebuffer.hpp"
#include "kernel/driver/e1000/e1000.hpp"
#include "kernel/driver/pci/pci.hpp"
#include "kernel/driver/ahci/ahci.hpp"
#include "kernel/driver/xhci/xhci.hpp"
#include "kernel/driver/pic/pic.hpp"
#include "kernel/driver/pic/timer/pit.hpp"
#include "kernel/filesys/gpt/gpt.hpp"
#include "kernel/filesys/rootfs/rootfs.hpp"
#include "kernel/filesys/vfs/vfs.hpp"
#include "kernel/filesys/fat32/fat32.hpp"
#include "kernel/filesys/devfs/devfs.hpp"
#include "kernel/intidt/idt.hpp"
#include "kernel/log/fbcon/fbcon.hpp"
#include "kernel/log/printk/printk.hpp"
#include "kernel/mm/kmalloc/kmalloc.hpp"
#include "kernel/mm/mm.hpp"
#include "firmware/acpi/madt/smpmod/smp.hpp"
#include "debug.hpp"
#include "kernel/mm/shm/shm.hpp"
#include "rossys.hpp"
#include <filesystem/filesystem.hpp>
#include "kernel/filesys/pmos/partition_manager.hpp"
#include <string.hpp>
#include <userland/syscall.hpp>
#include "kernel/task/task.hpp"
#include "rostime.hpp"
#include <network/ethernet.hpp>
#include <network/dns.hpp>
#include <network/tcp.hpp>
#include <network/udp.hpp>
#include <network/dhcp.hpp>
#include "kernel/driver/e1000/e1000.hpp"
#include "kernel/filesys/pipefs/pipe.hpp"
#include <../kernel/mod/module_manager.hpp>
#include <export_sym.hpp>

void DumpSymbols() {
    for (KernelSymbol* s = __ksymtab_start; s < __ksymtab_end; s++) {
        const char* name = (const char*)resolve_symbol_address(&s->name_offset);
        uintptr_t addr   = resolve_symbol_address(&s->value_offset);
        
        Serial::Printf("Symbol: %s at %p\n", name, (void*)addr);
    }
}

ABI_C VOID IdleLoop(VOID*){
    while(TRUE){
        Tasking::SchedulerYield();
    }
}

ABI_C NORET void KernelMain(VOID*)
{ 
    Arch::ASM::EnableSSE();
    Arch::ASM::FPU_Init();
    FB::Init();
    Printk::Init();
    Printk::Write(Printk::Level::LOG_INFO, "FPU Initalized.\n");
    // Initialize PIC and PIT early so we can use PIT as a calibration
    // source for APIC timer calibration when ACPI brings up LAPIC.

    // Now that LAPIC timer calibrated, PIT ticks flowing, interrupts enabled,
    // and IOAPIC/LAPIC initialized, start Application Processors.
    //AACPI::LAPIC::SMP::InitSMP();

    ROOTFS::InitROOTFS();
    DEVFS::Init();
    PipeFS::Init();

    BootInfoPrint();

    // Initialize PCI and its drivers
    PCI::IntializePCIDrivers();

    PartitionManager::InitializePM();

    EXT2::InitializeEXT2Driver();
    VFSManager::RegisterFileSystem("FAT32", []()->FileSystem* { return new FAT32FileSystem(); });
    GPTFS::ScanAllDevices();

    Userland::Syscall_Init();
    SharedMemoryManager::Init();

    const char* DeviceName = "nvme0n1p2";

    Partition *P = PartitionManager::FindPartitionByDeviceName(DeviceName);
    if(P && P->Mount()){
        FileSystem *EXT2FS = P->GetFilesystem();
        if(EXT2FS){
            ROOTFS::GetRootFS()->SetBackingFileSystem(EXT2FS);
            Printk::Write(Printk::Level::LOG_INFO, "KernelMain: Successfully mounted partition %s as RootFS Backing Filesystem.\n", DeviceName);
        }
    } else {
        Printk::Write(Printk::Level::LOG_ERR, "KernelMain: Failed to mount partition sda2.\n"); 
    }

    DumpSymbols();

    // TES DriverExported
    File *f = VFSManager::Open("/driver/mm.ko", O_RDONLY);
    if(f){
        U64 FileSize = f->Node->FileSize;
        VOID *FileBuffer = Kmalloc::Alloc(FileSize);
        if(FileBuffer){
            U64 ReadBytes = VFSManager::Read(f, (U8*)FileBuffer, FileSize);
            if(ReadBytes == FileSize){
                Printk::Write(Printk::Level::LOG_INFO, "KernelMain: Successfully read mm.ko into memory, size %llu bytes.\n", (unsigned long long)ReadBytes);
                int status = ModuleManager::LoadModuleAndRun(FileBuffer, 0);
                if(status == 0){
                    Printk::Write(Printk::Level::LOG_INFO, "KernelMain: mm.ko loaded and initialized successfully.\n");
                } else {
                    Printk::Write(Printk::Level::LOG_ERR, "KernelMain: mm.ko failed to initialize (status code %d).\n", status);
                }
            } else {
                Printk::Write(Printk::Level::LOG_ERR, "KernelMain: Failed to read full mm.ko (read %llu of %llu bytes).\n",
                              (unsigned long long)ReadBytes, (unsigned long long)FileSize);
            }
            Kmalloc::Free(FileBuffer);
        } else {
            Printk::Write(Printk::Level::LOG_ERR, "KernelMain: Failed to allocate memory for mm.ko (size %llu bytes).\n", (unsigned long long)FileSize);
        }
        VFSManager::Close(f);
    } else {
        Printk::Write(Printk::Level::LOG_ERR, "KernelMain: Failed to open /driver/mm.ko\n");
    }

    {
        File *F = VFSManager::Open("/init.elf", O_RDONLY);
        if(F){
            Printk::Write(Printk::Level::LOG_INFO, "KernelMain: Successfully opened /init.elf, size %llu bytes.\n", (unsigned long long)F->Node->FileSize);
            U64 ELFSize = F->Node->FileSize;
            VOID *ELFImage = Kmalloc::Alloc(ELFSize);
            if(!ELFImage){
                Printk::Write(Printk::Level::LOG_ERR, "KernelMain: failed to allocate memory for init.elf\n");
                goto halt;
            }
            U64 ReadBytes = VFSManager::Read(F, (U8*)ELFImage, ELFSize);
            if(ReadBytes != ELFSize){
                Printk::Write(Printk::Level::LOG_ERR, "KernelMain: failed to read full init.elf (read %llu of %llu)\n",
                              (unsigned long long)ReadBytes, (unsigned long long)ELFSize);
                Kmalloc::Free(ELFImage);
                goto halt;
            }

            // HACK: bisa terjadi undefined behavior karena mengubah task struct saat scheduler berjalan. jadinya,
            // kita cli lalu sti saat task sudah selesai di rubah.
            Arch::ASM::Cli();

            Tasking::CreateUserTask("init", ELFImage);

            Tasking::Task *InitTask = Tasking::GetTaskPID(2);
            if(InitTask){
                InitTask->IsCriticalProc = TRUE; // Tandai init sebagai critical process
                InitTask->IsEssentialSystem = TRUE; // Tandai init sebagai essential system
                InitTask->IsSudoOrAdmin = TRUE; // Beri akses sudo/admin ke init
            }
            Kmalloc::Free(ELFImage);
            Printk::Write(Printk::Level::LOG_INFO, "KernelMain: init.elf loaded and task created successfully.\n");
            VFSManager::Close(F);
            Printk::Write(Printk::Level::LOG_INFO, "KernelMain: Closed /init.elf file handle.\n");
            Arch::ASM::Sti();
        } else {
            Printk::Write(Printk::Level::LOG_ERR, "KernelMain: failed to open /mnt/part1/init.elf\n");
        }
    }

    UNUSED__ halt:
    
    // Main idle loop: poll serial and keyboard consumers so IRQ-driven
    // producers are serviced. This keeps IRQ handlers minimal (they only
    // enqueue) while the main loop does I/O and console rendering.
    for (;;) {
        Tasking::SchedulerYield(); // Yield to scheduler to run other tasks (like init)
    }
    
}
