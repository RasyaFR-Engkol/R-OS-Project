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
#include "../../../filesys/devfs/std_devices.hpp"

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
        static char TranslateScancode(U8 code, bool shift, BOOL extended) {
            // Unshifted map (Set 1 common)
            [[maybe_unused]] static const char unshift[256] = {0};
            [[maybe_unused]] static const char shiftmap[256] = {0};
            if (extended) {
                // Mapping Tombol Spesial (0xE0 + Code)
                switch (code) {
                    case 0x48: return (char)0x80; // UP
                    case 0x50: return (char)0x81; // DOWN
                    case 0x4B: return (char)0x82; // LEFT
                    case 0x4D: return (char)0x83; // RIGHT
                    case 0x53: return (char)0x7F; // DELETE (Map ke ASCII DEL)
                    default: return 0;
                }
            }
            
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

        // Di implementasi
        void InjectScancode(U8 sc) {
            // 1. Masukin ring buffer (Logic copas dari Keyboard_OnIrq tapi tanpa Inb(0x60))
            unsigned head = sc_head;
            unsigned next = (head + 1) & KB_BUF_MASK;
            if (next != sc_tail) {
                scancode_buf[head] = sc;
                asm volatile ("mfence" ::: "memory");
                sc_head = next;
            }

            // 2. Langsung proses (biar responsive)
            Poll();
        }

        STATIC BOOL IsExtended = FALSE;
        void Poll() {
            while (sc_tail != sc_head) {
                // Pop
                unsigned tail = sc_tail;
                U8 sc = scancode_buf[tail];
                asm volatile ("mfence" ::: "memory");
                sc_tail = (tail + 1) & KB_BUF_MASK;

                bool is_make = !(sc & 0x80);
                U8 code = sc & 0x7F;

                if(sc == 0xE0){
                    IsExtended = TRUE;
                    continue;
                }

                if (code == 0x1D){
                    ctrl_down = is_make;
                    IsExtended = FALSE;
                    continue;
                }


                // Update Shift state
                if (code == 0x2A || code == 0x36) {
                    shift_down = is_make;
                    IsExtended = FALSE;
                    continue;
                }

                char ch = TranslateScancode(code, shift_down, IsExtended);

                if (!is_make) {
                    // Break code: reset extended flag and skip further processing
                    IsExtended = FALSE;
                    continue;
                }

                IsExtended = FALSE;

                if (ch) {
                    // Regular printable character handling
                    // If Ctrl is held, map printable ASCII to control codes.
                    // e.g. Ctrl+A -> 0x01, Ctrl+C -> 0x03 (SIGINT is handled
                    // earlier as a special case and will not reach here).
                    if (ctrl_down) {
                        unsigned char uc = (unsigned char)ch;
                        // Map according to ASCII control mapping (mask lower 5 bits)
                        // This converts letters to 1..26, and common Ctrl combos.
                        uc = uc & 0x1F;
                        ch = (char)uc;
                    }
                    // 1. Echo to Kernel Console (FB & Serial)
                    UNUSED__ CHAR8 buf[2] = { (CHAR8)ch, 0 };

                    // 2. Store in ASCII Buffer for Stdin
                    unsigned next_ascii = (ascii_head + 1) % ASCII_BUF_SIZE;
                    if (next_ascii != ascii_tail) {
                        ascii_buf[ascii_head] = ch;
                        ascii_head = next_ascii;
                    }

                    // 2.5 Store to TTY
                    if (StdDvc::ListeningTTY != nullptr) {
                        StdDvc::ListeningTTY->OnInput(ch);
                    }

                    // 3. Wake up waiting task
                    if (WaitingTask) {
                        WaitingTask->State = Tasking::TaskState::READY;
                        
                        // --- TAMBAHAN PENTING (IO BOOST) ---
                        // Karena task ini bangun dari IO (Interactive), dia harus prioritas tertinggi!
                        WaitingTask->vruntime = Tasking::MinVRuntime - 1;
                        
                        // Opsional: Set flag global biar Scheduler tau ada yang urgent
                        Tasking::ForceReschedule = TRUE; 
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
                Printk::Write(Printk::Level::LOG_INFO, "Keyboard PIC: No input available, blocking task PID %llu\n", current ? current->pid : 0);
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

        void FlushBuffer(){
            // Clear the ASCII input buffer in an IRQ-safe manner
            LOCKRFLAGS irq = Arch::SaveAndDisableInterrupts();
            ascii_head = ascii_tail; // empty buffer
            Arch::RestoreInterrupts(irq);
        }
    }
}