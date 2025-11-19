#define PRINTK_MODULE_NAME "PITTimer"
#include <rosval.h>
#include "pit.hpp"
#include "../../pic/pic.hpp"
#include <port.hpp>
#include <logging.hpp>
#include <serial.hpp>
#include "../../../intidt/idt.hpp"

/* module name provided via PRINTK_MODULE_NAME */

namespace PIT {
    U16 PITReload = 0;
    volatile U64 ticks = 0; 
    static void PIT_OnIrq(void *context) {

        // ini memang timer handler juga. tapi ga bakal
        // pernah kepake untuk scheduler karena kita
        // pake LAPIC timer sekarang.
        // tapi biar konsisten, kita tambahin context
        // supaya sesuai prototype handler IDT.
        
        ticks = ticks + 1;
    }

    void InitializePIT(U32 hz){
        using namespace Port;
        // PIT input clock
        const U32 PIT_BASE = 1193182U;
        if (hz == 0) hz = 100; // avoid div0
        U32 Divisor = PIT_BASE / hz;
        PITReload = (U16)Divisor;

        // Set PIT to mode 3 (square wave), lobyte/hibyte
        Outb(0x43, 0x36);
        Outb(0x40, (U8)(Divisor & 0xFF));
        Outb(0x40, (U8)((Divisor >> 8) & 0xFF));

        // Unmask IRQ0 on PIC (clear bit0)
        U8 mask_master = Inb(0x21);
        mask_master &= ~(1 << 0); // clear IRQ0 mask
        Outb(0x21, mask_master);

        // Register the timer interrupt handler on vector 0x20 (IRQ0)
        IDT::RegisterInterruptHandler(0x20, PIT_OnIrq);
        Printk::Write(Printk::Level::LOG_INFO, " Initialize PIT at %u Hz (Divisor=%u)\n", hz, Divisor);
    }

}