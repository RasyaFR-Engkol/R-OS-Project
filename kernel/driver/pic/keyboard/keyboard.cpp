#include <rosval.h>
#include <rossys.hpp>
#include <serial.hpp>
#include "../../../intidt/idt.hpp"
#include <logging.hpp>
#include <port.hpp>
#include <framebuffer.hpp>

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

    // Modifier state (handled by consumer, not IRQ)
    static bool shift_down = false;

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

        static void Keyboard_OnIrq(){
            using namespace Port;
            // Read scancode from PS/2 data port
            U8 sc = Inb(0x60);

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

            // Acknowledge to PIC as soon as possible
            PIC::SendEOI(1); // IRQ1 for keyboard
        }

        // Consumer: process queued scancodes. Call this periodically from the
        // main loop / console task to avoid doing work in IRQ context.
        void Poll() {
            while (sc_tail != sc_head) {
                // Pop
                unsigned tail = sc_tail;
                U8 sc = scancode_buf[tail];
                asm volatile ("mfence" ::: "memory");
                sc_tail = (tail + 1) & KB_BUF_MASK;

                bool is_make = !(sc & 0x80);
                U8 code = sc & 0x7F;

                // Update Shift state
                if (code == 0x2A || code == 0x36) {
                    shift_down = is_make;
                    continue;
                }

                if (!is_make) continue; // ignore releases for printable handling

                char ch = TranslateScancode(code, shift_down);
                if (ch) {
                    CHAR8 buf[2] = { (CHAR8)ch, 0 };
                    FBConsole::WriteString(buf);
                    if (ch == '\b') {
                        Serial::SerialPutC('\b');
                        Serial::SerialPutC(' ');
                        Serial::SerialPutC('\b');
                    } else {
                        Serial::SerialPutC(ch);
                    }
                }
            }
        }

        void InitializeKeyboardPIC(){
            // Register the keyboard interrupt handler on vector 0x20 + 1 = 0x21 (IRQ1)
            IDT::RegisterInterruptHandler(0x21, Keyboard_OnIrq);
            Printk::Write(Printk::Level::LOG_INFO, "[PIC-KEYBOARD] Keyboard PIC (PS/2) Initialized\n");
        }
    }
}