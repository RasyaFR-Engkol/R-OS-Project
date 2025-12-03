#include <task.hpp>
#include <rosval.h>
#include "../../../log/fbcon/fbcon.hpp"
#include "inputd.hpp"
#include "rossys.hpp"
#include "serial.hpp"
#include "../../../driver/pic/pic.hpp"
#include <logging.hpp>
#include "../../../driver/xhci/xhci.hpp"

namespace Tasking{
    VOID InputDaemonTask(){
        while(TRUE){
            FBConsole::UpdateCursor();
            Serial::PollToConsoles();

            bool didWork = false; // Flag buat nandain kita kerja atau gabut

            InputEvent ev;
            while(InputManager::PopEvent(ev)){

                didWork = true;
                if(ev.Type == InputEventType::MOUSE_MOVE){
                    Printk::Write(Printk::Level::LOG_DEBUG, "InputDaemon: Mouse Move dX=%d dY=%d\n", ev.Mouse.dX, ev.Mouse.dY);
                }
                else if(ev.Type == InputEventType::KEYBOARD_PRESS){
                    PIC::Keyboard::InjectScancode(ev.Keycode.Scancode);
                } else if(ev.Type == InputEventType::KEYBOARD_RELEASE){
                    // === FIX: Inject Break Code ===
                    // Tambahkan 0x80 (bit 7 nyala) untuk nandain Release
                    PIC::Keyboard::InjectScancode(ev.Keycode.Scancode | 0x80);
                }
            }

            xHCI::CheckPendingMSC(xHCI::g_xhci_controllers[0]); // Cek controller 0 saja untuk sekarang

            if (SchedulerActive) {
            if (didWork) {
                // Kalau kita abis kerja keras, kasih kesempatan task lain jalan (Yield).
                // Gak perlu Halt, karena mungkin masih ada event numpuk.
                Tasking::SchedulerYield();
            } else {
                // Kalau kita GABUT (gak ada event), Tidur aja (Halt).
                // Ini biar CPU adem. Nanti bangun pas ada interrupt (Timer/Keyboard/Mouse).
                Arch::ASM::Hti(); // Pastikan Hti ini: STI + HLT (Enable Int lalu Halt)
            }
        } else {
            Arch::ASM::HaltCPU();
        }
        }
    }
}