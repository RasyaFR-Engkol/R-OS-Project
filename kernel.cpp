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
#include "kernel/intidt/idt.hpp"
#include "kernel/log/printk/printk.hpp"
#include "kernel/mm/kmalloc/kmalloc.hpp"
#include "kernel/mm/mm.hpp"
#include "firmware/acpi/madt/smpmod/smp.hpp"
#include "debug.hpp"
#include "rossys.hpp"

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
    xHCI::InterruptBurstTest(5);

    // AHCI read test: try LBA0 from first available SATA port and hex dump
    for (int i = 0; i < AHCI::g_ahci_controller_count; ++i) {
        AHCI::AHCIDriver &drv = AHCI::g_ahci_controllers[i];
        if (!drv.regs) continue;
        U32 pi = drv.regs->pi;
        for (int port = 0; port < 32; ++port) {
            if ((pi & (1u << port)) == 0) continue;
            Serial::Printf("[AHCI TEST] Trying READ LBA0 on ctrl %d port %d...\n", i, port);
            PageAlloc::DMAAlloc::DMABuffer *buf = nullptr;
            if (AHCI::ReadSectors(drv, port, /*lba*/0, /*count*/1, &buf)) {
                Serial::Write("[AHCI TEST] READ LBA0 OK, dumping 512B (phys+virt views):\n");
                // Sanity: verify virt->phys translation matches the DMA phys address
                UPTR chkPhys=0; U64 chkFlags=0; SIZE_T lvl=0;
                if (Debug::VirtToPhys((UPTR)buf->VirtAddr, &chkPhys, &chkFlags, &lvl)) {
                    Serial::Printf("[AHCI TEST] DMA buf virt=%p -> phys=%p flags=%llx lvl=%u\n",
                                   (void*)(uintptr_t)buf->VirtAddr, (void*)(uintptr_t)chkPhys,
                                   (unsigned long long)chkFlags, (unsigned)lvl);
                } else {
                    Serial::Printf("[AHCI TEST] VirtToPhys FAILED for %p\n", (void*)(uintptr_t)buf->VirtAddr);
                }
                // View 1: base printed as physical address
                Debug::HexDump((void*)(uintptr_t)buf->VirtAddr, 512, 16, buf->PhysAddr, true);
                // View 2: base printed as the virtual pointer
                Debug::HexDump((void*)(uintptr_t)buf->VirtAddr, 512, 16, 0, true);
                PageAlloc::DMAAlloc::FreeDMABuffer(buf);
                // Only test first successful port
                i = AHCI::g_ahci_controller_count; // break outer
                break;
            } else {
                Serial::Write("[AHCI TEST] READ LBA0 failed\n");
            }
        }
    }

    // Main idle loop: poll serial and keyboard consumers so IRQ-driven
    // producers are serviced. This keeps IRQ handlers minimal (they only
    // enqueue) while the main loop does I/O and console rendering.
    for (;;) {
        // Drain any incoming serial characters (mirrors to FB/serial)
        Serial::PollToConsoles();
        // Process queued keyboard scancodes and echo them to consoles
        PIC::Keyboard::Poll();
        // Halt until next interrupt to reduce CPU usage
        asm volatile ("hlt");
    }
    
}
