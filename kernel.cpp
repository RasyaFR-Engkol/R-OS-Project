// Minimal kernel entry implementation that prints to serial (COM1)
// This helps verify that we've actually jumped into 64-bit kernel_main.

#include <stdint.h>
#include <serial.hpp>
#include <rosval.h>
#include <bootinfo.h>
#include "firmware/acpi/acpi.hpp"
#include "firmware/acpi/driver/timer/timer.hpp"
#include "framebuffer.hpp"
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
#include "kernel/driver/fb/fbcon_driver.hpp"
#include "kernel/log/printk/printk.hpp"
#include "kernel/mm/kmalloc/kmalloc.hpp"
#include "kernel/mm/mm.hpp"
#include "firmware/acpi/madt/smpmod/smp.hpp"
#include "debug.hpp"
#include "rossys.hpp"
#include <filesystem/filesystem.hpp>
#include "kernel/filesys/pmos/partition_manager.hpp"
#include <string.hpp>
#include <userland/syscall.hpp>
#include "kernel/task/task.hpp"
#include "rostime.hpp"

// Debug stress test entry (implemented in tools/debug/ext2_stress.cpp)
ABI_C int main_debug_ext2_stress(int argc, char** argv);

VOID TestScheduler(){
    while(1){
        Arch::ASM::CPURelax();
    }
}

VOID TestScheduler2(){
    while(1){
        Arch::ASM::CPURelax();
    }
}

ABI_C NORET void KernelMain()
{ 
    IDT::InitializeIDT();
    // Initialize PIC and PIT early so we can use PIT as a calibration
    // source for APIC timer calibration when ACPI brings up LAPIC.
    PIC::InitializePIC();
    PIC::Keyboard::InitializeKeyboardPIC();
    PIT::InitializePIT(100); // set PIT to 100 Hz (calibration/reference)
    // Enable IRQ-driven serial input (COM1 IRQ4)
    Serial::EnableIRQInput();

    // Initialize ACPI/MADT which will parse tables and initialize LAPIC/IOAPIC
    // (but do not start the LAPIC timer yet; we need interrupts enabled to
    // calibrate it against the PIT).
    ACPI::Initialize();

    // Enable interrupts so PIT IRQs will increment PIT::ticks for calibration
    Arch::Sti();

    // Calibrate and start LAPIC timer at 100 Hz (uses PIT ticks)
    // Use a distinct vector for LAPIC timer (not IRQ0/PIT vector 0x20)
    // to avoid conflicts with the PIT handler while calibration runs.
    ACPI::Timer::InitializeLapicTimer(CONFIG_TIMER_HEXA_GLOBAL, 100, TRUE);

    // Now mask and disable legacy PIC hardware while interrupts are briefly
    // disabled inside the call. After that, re-enable interrupts so LAPIC
    // delivered interrupts are accepted.
    PIC::DisableIRQWhileAndMaskOldPIC();
    Arch::Sti();

    // Now that LAPIC timer calibrated, PIT ticks flowing, interrupts enabled,
    // and IOAPIC/LAPIC initialized, start Application Processors.
    ACPI::LAPIC::SMP::InitSMP();

    BootInfoPrint();

    ROOTFS::InitROOTFS();
    DEVFS::Init();
    FB::Init();
    Printk::Init();

    // Initialize PCI and its drivers
    PCI::IntializePCIDrivers();

    // xHCI test: send multiple Enable Slot commands (no NOOP, no polling) to verify repeated MSIs.
    //xHCI::InterruptBurstTest(5); Disable this for now to reduce noise.

    // AHCI read test: try LBA0 from first available SATA port and hex dump
    //AHCI::TestReadLBA0();

    // Mount an in-kernel DevFS at /dev and register framebuffer device there

    GPTFS::InitFs();

    Userland::Syscall_Init();

    Tasking::CreateKThread(TestScheduler);

    {
        File *F = VFSManager::Open("/mnt/part1/init.elf");
        if(F){
            U64 ELFSize = F->FileSize;
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

            Tasking::CreateUserTask("init", ELFImage);
            Kmalloc::Free(ELFImage);
            VFSManager::Close(F);
        } else {
            Printk::Write(Printk::Level::LOG_ERR, "KernelMain: failed to open /mnt/part1/init.elf\n");
        }
    }

    Tasking::SchedulerStart();

    UNUSED__ halt:
    
    // Main idle loop: poll serial and keyboard consumers so IRQ-driven
    // producers are serviced. This keeps IRQ handlers minimal (they only
    // enqueue) while the main loop does I/O and console rendering.
    for (;;) {
        FBConsole::UpdateCursor();
        // Drain any incoming serial characters (mirrors to FB/serial)
        Serial::PollToConsoles();
        // Process queued keyboard scancodes and echo them to consoles
        PIC::Keyboard::Poll();
        // Halt until next interrupt to reduce CPU usage
        Arch::ASM::HaltCPU();
    }
    
}
