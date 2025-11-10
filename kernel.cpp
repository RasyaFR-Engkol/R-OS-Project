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
#include "kernel/filesys/vfs/vfs.hpp"
#include "kernel/filesys/fat32/fat32.hpp"
#include "kernel/intidt/idt.hpp"
#include "kernel/log/fbcon/fbcon.hpp"
#include "kernel/log/printk/printk.hpp"
#include "kernel/mm/kmalloc/kmalloc.hpp"
#include "kernel/mm/mm.hpp"
#include "firmware/acpi/madt/smpmod/smp.hpp"
#include "debug.hpp"
#include "rossys.hpp"
#include <filesystem/filesystem.hpp>
#include "kernel/filesys/pmos/partition_manager.hpp"
#include <string.hpp>

ABI_C void KernelMain()
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
    ACPI::Timer::InitializeLapicTimer(0x20, 100, TRUE);

    // Now mask and disable legacy PIC hardware while interrupts are briefly
    // disabled inside the call. After that, re-enable interrupts so LAPIC
    // delivered interrupts are accepted.
    PIC::DisableIRQWhileAndMaskOldPIC();
    Arch::Sti();

    // Now that LAPIC timer calibrated, PIT ticks flowing, interrupts enabled,
    // and IOAPIC/LAPIC initialized, start Application Processors.
    ACPI::LAPIC::SMP::InitSMP();

    BootInfoPrint();

    FB::Init();
    Printk::Init();

    // Initialize PCI and its drivers
    PCI::IntializePCIDrivers();

    // xHCI test: send multiple Enable Slot commands (no NOOP, no polling) to verify repeated MSIs.
    //xHCI::InterruptBurstTest(5); Disable this for now to reduce noise.

    // AHCI read test: try LBA0 from first available SATA port and hex dump
    AHCI::TestReadLBA0();

    // Register filesystem drivers BEFORE initializing GPT/partitions
    VFSManager::RegisterFileSystem("FAT32", []()->FileSystem* { return new FAT32FileSystem(); });

    GPTFS::InitFs();

    // Demo 2: create and write a small test file "/test.txt" with Virtual File System
    {
        const char *TestVFSPath = "/mnt/part0/testVFS.txt";
        File *F = VFSManager::Create(TestVFSPath);
        if(F){
            const char *TestingDATA = "Hello from R-OS kernel VFS!\nThis is a test file created using VFS and FAT32 driver.\n";
            SIZE_T ToWrite = String::Strlen(TestingDATA);
            U32 ByteWritten = VFSManager::Write(F, (U8*)TestingDATA, (U32)ToWrite);
            Printk::Write(Printk::Level::LOG_INFO, "Wrote %u/%llu bytes to %s\n", ByteWritten, ToWrite, TestVFSPath);
            VFSManager::Close(F);
        } else {
            Printk::Write(Printk::Level::LOG_ERR, "Failed to create test file %s\n", TestVFSPath);
        }
    }

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
