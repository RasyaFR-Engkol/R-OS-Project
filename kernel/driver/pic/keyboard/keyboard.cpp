#include <rosval.h>
#include <rossys.hpp>
#include <serial.hpp>
#include "../../../intidt/idt.hpp"
#include "rng/entrophy.hpp"
#define PRINTK_MODULE_NAME "PICKeyboard"
#include <logging.hpp>
#include <port.hpp>
#include <framebuffer.hpp>
#include <task.hpp>

/* module name provided via PRINTK_MODULE_NAME */

namespace PIC{
    namespace Keyboard{
    // Simple PS/2 scancode ring buffer (single-producer (IRQ) / single-consumer).
    // Keep size a power-of-two so we can mask indexes quickly.
    static constexpr unsigned KB_BUF_SIZE = 64;
    static constexpr unsigned KB_BUF_MASK = KB_BUF_SIZE - 1;
    static volatile U8 scancode_buf[KB_BUF_SIZE];
    // Use wider indices to avoid accidental tearing; producer (IRQ) updates
    // head, consumer updates tail. Both may read the other's index.
    static volatile unsigned sc_head = 0;
    static volatile unsigned sc_tail = 0;

    // ASCII Buffer for Stdin
    static constexpr unsigned ASCII_BUF_SIZE = 128;
    static char ascii_buf[ASCII_BUF_SIZE];
    static volatile unsigned ascii_head = 0;
    static volatile unsigned ascii_tail = 0;
    static Tasking::Task* WaitingTask = nullptr;

    // Modifier state (handled by consumer, not IRQ)
    static bool shift_down = false;
    static bool ctrl_down = false;

        // Translate a PS/2 Set 1 scancode (make code) to ASCII; returns 0 if not printable
        static char TranslateScancode(U8 code, bool shift) {
            // Unshifted map (Set 1 common)
            [[maybe_unused]] static const char unshift[256] = {0};
            [[maybe_unused]] static const char shiftmap[256] = {0};
            
            // We'll build minimal mapping inline for frequently used keys
            switch (code) {
                case 0x02: return shift ? '!' : '1';
                case 0x03: return shift ? '@' : '2';
                case 0x04: return shift ? '#' : '3';
                case 0x05: return shift ? '$' : '4';
                case 0x06: return shift ? '%' : '5';
                case 0x07: return shift ? '^' : '6';
                case 0x08: return shift ? '&' : '7';
                case 0x09: return shift ? '*' : '8';
                case 0x0A: return shift ? '(' : '9';
                case 0x0B: return shift ? ')' : '0';
                case 0x0C: return shift ? '_' : '-';
                case 0x0D: return shift ? '+' : '=';
                case 0x0E: return '\b';
                case 0x0F: return '\t';
                case 0x10: return shift ? 'Q' : 'q';
                case 0x11: return shift ? 'W' : 'w';
                case 0x12: return shift ? 'E' : 'e';
                case 0x13: return shift ? 'R' : 'r';
                case 0x14: return shift ? 'T' : 't';
                case 0x15: return shift ? 'Y' : 'y';
                case 0x16: return shift ? 'U' : 'u';
                case 0x17: return shift ? 'I' : 'i';
                case 0x18: return shift ? 'O' : 'o';
                case 0x19: return shift ? 'P' : 'p';
                case 0x1A: return shift ? '{' : '[';
                case 0x1B: return shift ? '}' : ']';
                case 0x1C: return '\n'; // Enter
                case 0x1E: return shift ? 'A' : 'a';
                case 0x1F: return shift ? 'S' : 's';
                case 0x20: return shift ? 'D' : 'd';
                case 0x21: return shift ? 'F' : 'f';
                case 0x22: return shift ? 'G' : 'g';
                case 0x23: return shift ? 'H' : 'h';
                case 0x24: return shift ? 'J' : 'j';
                case 0x25: return shift ? 'K' : 'k';
                case 0x26: return shift ? 'L' : 'l';
                case 0x27: return shift ? ':' : ';';
                case 0x28: return shift ? '"' : '\'';
                case 0x29: return shift ? '~' : '`';
                case 0x2B: return shift ? '|' : '\\';
                case 0x2C: return shift ? 'Z' : 'z';
                case 0x2D: return shift ? 'X' : 'x';
                case 0x2E: return shift ? 'C' : 'c';
                case 0x2F: return shift ? 'V' : 'v';
                case 0x30: return shift ? 'B' : 'b';
                case 0x31: return shift ? 'N' : 'n';
                case 0x32: return shift ? 'M' : 'm';
                case 0x33: return shift ? '<' : ',';
                case 0x34: return shift ? '>' : '.';
                case 0x35: return shift ? '?' : '/';
                case 0x39: return ' ';
                default: return 0;
            }
        }

        // Forward declaration
        void Poll();

        static void Keyboard_OnIrq(void *context){
            using namespace Port;
            // Read scancode from PS/2 data port
            U8 sc = Inb(0x60);

            U64 TimeStamp = Arch::ASM::RdTSC();
            EntrophySystem::AddEntrophy((U32)TimeStamp ^ sc);

            // Push into ring buffer if not full. Keep IRQ handler minimal and
            // fast: only enqueue the scancode and send EOI. Heavy work (text
            // rendering, serial I/O) is deferred to Poll() which runs outside
            // IRQ context.
            unsigned head = sc_head;
            unsigned next = (head + 1) & KB_BUF_MASK;
            if (next != sc_tail) {
                scancode_buf[head] = sc;
                // Make sure the write to buffer is visible before we publish
                // the new head value.
                asm volatile ("mfence" ::: "memory");
                sc_head = next;
            } else {
                // buffer full, drop scancode (could increment a drop counter)
            }

            // Process the scancode immediately (User Request)
            Poll();

            // Acknowledge to PIC as soon as possible
            // EOI is handled by the central IRQ dispatcher (IrqDispatch).
            // Do not call PIC::SendEOI here to avoid unsafe I/O from IRQ handlers
            // and to allow LAPIC-based delivery to work correctly.
        }

        // Consumer: process queued scancodes. Call this periodically from the
        // main loop / console task to avoid doing work in IRQ context.

        void NotifyTaskDied(Tasking::Task* t) {
            if (WaitingTask == t) {
                WaitingTask = nullptr; // Lupakan dia!
            }
        }

        void Poll() {
            while (sc_tail != sc_head) {
                // Pop
                unsigned tail = sc_tail;
                U8 sc = scancode_buf[tail];
                asm volatile ("mfence" ::: "memory");
                sc_tail = (tail + 1) & KB_BUF_MASK;

                bool is_make = !(sc & 0x80);
                U8 code = sc & 0x7F;

                if (code == 0x1D){
                    ctrl_down = is_make;
                    continue;
                }


                // Update Shift state
                if (code == 0x2A || code == 0x36) {
                    shift_down = is_make;
                    continue;
                }

                if(ctrl_down && is_make && code == 0x2E){
                    VFSManager::Write(Printk::s_SerialConsoleFile, (U8*)"^C\n", 3);
                    VFSManager::Write(Printk::s_FrameConsoleFile, (U8*)"^C\n", 3);
                    // CTRL + C detected
                    if(Tasking::g_ForegroundPID == (U64)-1){
                        Printk::Write(Printk::Level::LOG_WARNING, "Keyboard: No foreground PID set to send SIGINT\n");
                        continue;
                    }
                    Printk::Write(Printk::Level::LOG_INFO, "Keyboard: CTRL+C detected, sending SIGINT to foreground PID %llu\n", Tasking::g_ForegroundPID);
                    Tasking::Task* fgTask = Tasking::TaskArray[Tasking::g_ForegroundPID];
                    if(fgTask){

                        // siapa tau fgTask emang lagi punya PGID. jadi kita
                        // loop setiap task, dan kalo PGID nya sama dengan si
                        // fgTask, kita kirim signal juga.
                        U64 target_pgid = fgTask->PGID;
                        if(target_pgid != 0){
                            Printk::Write(Printk::Level::LOG_INFO, "Keyboard: Foreground task PID %llu has PGID %llu, sending SIGINT to all in group\n", fgTask->pid, target_pgid);
                            for(U64 i = 0; i < MAX_TASK; i++){
                                Tasking::Task* t = Tasking::TaskArray[i];
                                if(t && t->PGID == target_pgid){
                                    Printk::Write(Printk::Level::LOG_INFO, "Keyboard: Sending SIGINT to PID %llu in PGID %llu\n", t->pid, target_pgid);
                                    t->Signals |= (1 << 2); // SIGINT is signal number 2

                                    // kalo emang kasus lagi blocked, bangunin dia
                                    if(t->State == Tasking::TaskState::BLOCKED){
                                        t->State = Tasking::TaskState::READY;
                                    }
                                }
                            }
                        }

                        fgTask->Signals |= (1 << 2); // SIGINT is signal number 2

                        // kalo emang kasus lagi blocked, bangunin dia
                        if(fgTask->State == Tasking::TaskState::BLOCKED){
                            fgTask->State = Tasking::TaskState::READY;
                        }

                        // Yield CPU to let it handle signal ASAP
                        Tasking::SchedulerYield();
                    } else {
                        Printk::Write(Printk::Level::LOG_WARNING, "Keyboard: No foreground task with PID %llu to send SIGINT\n", Tasking::g_ForegroundPID);
                    }
                    continue;
                }

                if (!is_make) continue; // ignore releases for printable handling

                char ch = TranslateScancode(code, shift_down);
                if (ch) {
                    // 1. Echo to Kernel Console (FB & Serial)
                    UNUSED__ CHAR8 buf[2] = { (CHAR8)ch, 0 };
                    // ini matiin aja dah
                    //FBConsole::WriteString(buf);
                    if (ch == '\b') {
                        Serial::SerialPutC('\b');
                        Serial::SerialPutC(' ');
                        Serial::SerialPutC('\b');
                    } else {
                        Serial::SerialPutC(ch);
                    }

                    // 2. Store in ASCII Buffer for Stdin
                    unsigned next_ascii = (ascii_head + 1) % ASCII_BUF_SIZE;
                    if (next_ascii != ascii_tail) {
                        ascii_buf[ascii_head] = ch;
                        ascii_head = next_ascii;
                    }

                    // 3. Wake up waiting task
                    if (WaitingTask) {
                        Tasking::Task* taskToWake = WaitingTask;
                        WaitingTask = nullptr; // Clear BEFORE yielding to avoid race with re-sleeping task

                        taskToWake->State = Tasking::TaskState::READY;
                        
                        // LATENCY FIX (MLFQ Style):
                        // Boost priority to 0 (Highest) so it gets picked up immediately
                        // by the scheduler on the next tick or yield.
                        taskToWake->Priority = 0;
                        taskToWake->TimeSlice = Tasking::GetTimeSliceForPriority(0);
                        taskToWake->TimeUsedInPriority = 0;

                        // Force reschedule immediately to switch to this high-prio task
                        Tasking::SchedulerYield();
                    }
                }
            }
        }

        char GetChar() {
            while (true) {
                // Check buffer atomically
                LOCKRFLAGS irq = Arch::SaveAndDisableInterrupts();
                if (ascii_head != ascii_tail) {
                    char c = ascii_buf[ascii_tail];
                    ascii_tail = (ascii_tail + 1) % ASCII_BUF_SIZE;
                    Arch::RestoreInterrupts(irq);
                    return c;
                }
                
                // Buffer empty, sleep
                Tasking::Task* current = Tasking::GetCurrentTaskPtr();
                if (current) {
                    WaitingTask = current;
                    current->State = Tasking::TaskState::BLOCKED;
                }
                Arch::RestoreInterrupts(irq);
                
                // Yield CPU if we blocked
                if (current) Tasking::SchedulerYield();
            }
        }

        void InitializeKeyboardPIC(){
            // Register the keyboard interrupt handler on vector 0x20 + 1 = 0x21 (IRQ1)
            IDT::RegisterInterruptHandler(0x21, Keyboard_OnIrq);
            Printk::Write(Printk::Level::LOG_INFO, " Keyboard PIC (PS/2) Initialized\n");
        }

        U32 GetBufferCount(){
            if(ascii_head >= ascii_tail){
                return ascii_head - ascii_tail;
            } else {
                return ASCII_BUF_SIZE - (ascii_tail - ascii_head);
            }
        }
    }
}