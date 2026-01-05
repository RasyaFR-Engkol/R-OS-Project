#include "../printk/printk.hpp"
#include "rossys.hpp"
#include "rosval.h"
#include <string.hpp>
#include <framebuffer.hpp>
#include "../../log/fbcon/fbcon.hpp"

namespace Printk{

    VOID NORET Panic(const char *msg, ...){
        using namespace String;

        FBConsole::UpdateConsoleStatus(TRUE);
        FBConsole::ResetStateAndClearByColorParam(0xFF0000FF);

        VA_LIST argument;
        VA_STRT(argument, msg);

        CHAR8 Buffer[512];
        // 1. Bersihkan buffer dulu biar aman
        String::Memset(Buffer, 0, sizeof(Buffer)); 

        // Variable untuk melacak posisi kursor penulisan di dalam Buffer
        U64 offset = 0;
        U64 max_size = sizeof(Buffer);

        // --- STEP 2: Print Pesan Error (Format) ---
        // Lanjut nulis setelah header
        offset += VSPrint(Buffer + offset, max_size - offset, msg, argument);

        VA_END(argument);

        using namespace FBConsole;

        // --- Output ke Layar/Serial ---
        if(FBConsole::IsReady()){
            WriteString("\n\nSYSBRK\n\n");
            WriteString("A problem has been detected and RasyaOS has been halted to prevent damage to your computer.\n"
            "The first step to resolve this issue is to restart your computer. If this screen appears again, follow these steps:\n"
            "1. Download the RasyaOS ISO.\n"
            "2. Create a bootable USB drive from the ISO.\n"
            "3. Configure your BIOS settings to boot from the USB drive.\n"
            "4. Select 'Repair my computer' from the boot menu.\n\n"
            "NOTE: If you do not have access to a USB drive or another PC, perform an Unclean Restart Attempt 3 times to trigger the Internal Recovery menu.\n\n\n"
            "STOP MESSAGE: ");
            WriteString(Buffer);
        }
        
        Serial::Write(Buffer);

        Arch::ASM::Cli();
        while(1) Arch::ASM::HaltCPU();

        UNREACHABLE;
    }
}