#pragma once

#include <rosval.h>
#include "devfs.hpp"
#include <task.hpp>
#include "../../dev/circularbuffer.hpp"

//STDIN
#define FIONREAD 0x541B // Get bytes available in input buffer
#define TIOCSPGRP 0x5410 // Set Process Group (Kita pake buat Set PID dulu)
#define TIOCGPGRP 0x540F // Get Process Group

//STDOUT
#define TIOCGWINSZ 0x5413 // Command ID standar Linux

// Use the foreground PID from the Tasking namespace
// (declared in `kernel/task/task.hpp`)

#define DEFAULT_CONFIG_PID_START 100

typedef unsigned char  cc_t;
typedef unsigned int   speed_t;
typedef unsigned int   tcflag_t;
#define NCCS 32 // Jumlah Control Character Slots


struct termios {
    tcflag_t c_iflag;   // Input flags
    tcflag_t c_oflag;   // Output flags
    tcflag_t c_cflag;   // Control flags
    tcflag_t c_lflag;   // Local flags (ISIG, ICANON, ECHO ada di sini)
    cc_t     c_line;    // Line discipline (jarang dipake skrg)
    cc_t     c_cc[NCCS];// Control characters (Ctrl+C, Backspace, etc)
    speed_t  c_ispeed;  // Input speed
    speed_t  c_ospeed;  // Output speed
};

struct winsize {
    unsigned short ws_row;    // rows, in characters
    unsigned short ws_col;    // columns, in characters
    unsigned short ws_xpixel; // horizontal size, pixels (optional)
    unsigned short ws_ypixel; // vertical size, pixels (optional)
};

class TTY : public ICharDevice {
    private:
        BOOL m_OutputStopped;
        U64 ForegroundPGID;
        CHAR8 m_name[32];
        termios m_Termios;
        static const U32 LINE_BUFFER_SIZE = 1024;
        U8 m_LineBuffer[LINE_BUFFER_SIZE];
        U32 m_LineReadPos;  // Posisi user lagi baca sampai mana
        U32 m_LineWritePos; // Posisi user lagi ngetik sampai mana

    public:
        TTY();
        virtual ~TTY();

        virtual U32 Read(File *file, U8* buffer, U32 size) override; 
        virtual U32 Write(File *file, U8* buffer, U32 size) override;
        virtual const CHAR8* GetDeviceName() override { return m_name; }
        virtual INTN Ioctl(File* file, U32 command, U64 arg) override;
        virtual short Poll(File *file, short events) override;
        virtual VOID OnInput(unsigned char c);

        VOID SetForegroundPID(U64 pid) {
            Tasking::g_ForegroundPID = pid;
        }

        U64 GetForegroundPID() {
            return Tasking::g_ForegroundPID;
        }
};

class NullDevice : public ICharDevice {
    public:
        NullDevice(){}
        virtual ~NullDevice(){}

        virtual U32 Read(File *file, U8* buffer, U32 size) override {
            (void)buffer; (void)size; return 0; // EOF
        }
        virtual U32 Write(File *file, U8* buffer, U32 size) override {
            (void)buffer; return size; // discard data
        }
        virtual const CHAR8* GetDeviceName() override { return "null"; }
        virtual INTN Ioctl(File* file, U32 command, U64 arg) override {
            (void)file; (void)command; (void)arg; return -ROS_UNSUPPORTED;
        }
        virtual short Poll(File *file, short events) override {
            short revents = 0;
            // Null device selalu siap dibaca (hasilnya EOF immediate)
            if (events & POLLIN)  revents |= POLLIN;
            // Null device selalu siap ditulisi (data dibuang immediate)
            if (events & POLLOUT) revents |= POLLOUT;
            return revents;
        }
};

class RandomDevice : public ICharDevice{
    private: 
        U32 m_Seed;
        const char *m_name;
        U32 NextRand();
    public:
        RandomDevice(const CHAR8* Name);
        virtual ~RandomDevice(){}

        virtual U32 Read(File *file, U8* buffer, U32 size) override;
        virtual U32 Write(File *file, U8* buffer, U32 size) override;
        virtual const CHAR8* GetDeviceName() override { return m_name; }
        virtual INTN Ioctl(File* file, U32 command, U64 arg) override {
            (void)file; (void)command; (void)arg; return -ROS_UNSUPPORTED;
        }
        virtual short Poll(File *file, short events) override {
            short revents = 0;
            // Random stream gak pernah abis
            if (events & POLLIN)  revents |= POLLIN;
            // Seed mixing selalu bisa dilakukan
            if (events & POLLOUT) revents |= POLLOUT;
            return revents;
        }
};

namespace StdDvc{
    extern U64 TTYActive;
    extern TTY *ListeningTTY;
    VOID RegisterSTD(DevFS* devfs);

    struct PACKSTRUCT winsize {
        unsigned short ws_row;    // rows, in characters
        unsigned short ws_col;    // columns, in characters
        unsigned short ws_xpixel; // horizontal size, pixels (optional)
        unsigned short ws_ypixel; // vertical size, pixels (optional)
    };
}

// --- Basic Termios Get/Set ---
#define TCGETS      0x5401  // Get terminal attributes (struct termios)
#define TCSETS      0x5402  // Set terminal attributes
#define TCSETSW     0x5403  // Set attributes (wait for output drain)
#define TCSETSF     0x5404  // Set attributes (flush input first)

// --- Window Size (PENTING buat Vim/Text Editor) ---
#define TIOCGWINSZ  0x5413  // Get Window Size
#define TIOCSWINSZ  0x5414  // Set Window Size

// --- Process Group / Job Control (Buat Shell pipeline) ---
#define TIOCGPGRP   0x540F  // Get Foreground Process Group ID
#define TIOCSPGRP   0x5410  // Set Foreground Process Group ID

// --- Buffer Status ---
#define FIONREAD    0x541B  // Get bytes available in input buffer
// =============================================================
//  c_lflag (LOCAL FLAGS) - PALING PENTING BUAT LOGIC TTY
// =============================================================
#define ISIG    0x00001  // Enable signal generation (Ctrl+C -> SIGINT)
#define ICANON  0x00002  // Canonical mode (Tunggu Enter baru kirim data)
#define ECHO    0x00008  // Enable echo input character
#define ECHOE   0x00010  // Echo Erase (Backspace ngehapus visual huruf)
#define ECHOK   0x00020  // Echo Kill line
#define ECHONL  0x00040  // Echo Newline walau ECHO mati
#define NOFLSH  0x00080  // Disable flushing on SIGINT
#define TOSTOP  0x00100  // Send SIGTTOU for background output
#define IEXTEN  0x08000  // Enable extended input processing

// =============================================================
//  c_iflag (INPUT FLAGS) - Ngatur data pas masuk dari keyboard
// =============================================================
#define IGNBRK  0x00001  // Ignore break condition
#define BRKINT  0x00002  // Signal interrupt on break
#define IGNPAR  0x00004  // Ignore characters with parity errors
#define PARMRK  0x00008  // Mark parity errors
#define INPCK   0x00010  // Enable input parity check
#define ISTRIP  0x00020  // Strip 8th bit off characters
#define INLCR   0x00040  // Map NL to CR on input
#define IGNCR   0x00080  // Ignore CR
#define ICRNL   0x00100  // Map CR to NL on input (PENTING! Enter biasanya CR, harus jadi NL)
#define IXON    0x00400  // Enable start/stop output control (Ctrl+S / Ctrl+Q)

// =============================================================
//  c_oflag (OUTPUT FLAGS) - Ngatur data sebelum dikirim ke layar
// =============================================================
#define OPOST   0x00001  // Post-process output (wajib nyala buat proses di bawah)
#define ONLCR   0x00004  // Map NL to CR-NL (PENTING! \n jadi \r\n biar gak tangga)

// =============================================================
//  c_cflag (CONTROL FLAGS) - Biasanya buat Hardware Serial
// =============================================================
#define CSIZE   0x00030  // Character size mask
#define CS8     0x00030  // 8 bit
#define CREAD   0x00080  // Enable receiver
#define PARENB  0x00100  // Parity enable
#define HUPCL   0x00400  // Hangup on last close

// =============================================================
//  c_cc Indices (Control Characters) - Index Array
// =============================================================
#define VINTR   0   // Interrupt (Ctrl+C) -> Default: 0x03
#define VQUIT   1   // Quit (Ctrl+\)      -> Default: 0x1C
#define VERASE  2   // Erase (Backspace)  -> Default: 0x7F or 0x08
#define VKILL   3   // Kill (Ctrl+U)      -> Default: 0x15
#define VEOF    4   // End-of-file (Ctrl+D) -> Default: 0x04
#define VTIME   5   // Time to wait (Non-canonical read)
#define VMIN    6   // Min char to wait (Non-canonical read)
#define VSTART  8   // Start (Ctrl+Q)
#define VSTOP   9   // Stop (Ctrl+S)