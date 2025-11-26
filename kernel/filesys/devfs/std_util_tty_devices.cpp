#include <rossys.hpp>
#include <rosval.h>
#include <serial.hpp>
#include "../../driver/pic/pic.hpp"
#include "rng/entrophy.hpp"
#define PRINTK_MODULE_NAME "StdDvc"
#include <logging.hpp>
#include "std_devices.hpp"
#include "../../log/fbcon/fbcon.hpp"
#include "../../dev/devicemanager.hpp"
#include <task.hpp>

VOID TTY::OnInput(char c){
    if(c & 0x80) return;
    if (c == 0) return;

    UNUSED__ bool canonical = (m_Termios.c_lflag & ICANON);
    bool echo      = (m_Termios.c_lflag & ECHO);
    bool mapCRNL   = (m_Termios.c_iflag & ICRNL);

    if ((m_Termios.c_lflag & ISIG) && c == m_Termios.c_cc[VINTR]) {
        U64 fgPID = GetForegroundPID();
        
        // Kirim Signal ke Foreground Process (Target PID, bukan Group dulu biar aman)
        if (fgPID > 0) {
            Tasking::SetTaskSignal(fgPID, 2 /* SIGINT */, TRUE);
            
            // Opsional: Echo "^C"
            if (echo) {
                this->Write((U8*)"^C\n", 3);
            }
            
            // Flush buffer input baris ini (opsional, behavior unix)
            m_LineWritePos = 0; 
            m_LineReadPos = 0;
        }
        return; // JANGAN simpan karakter ini ke buffer
    }

    if (c == '\r' && mapCRNL) c = '\n';

    // 3. Handle Backspace
    if (c == '\b' || c == 0x7F) {
        if (m_LineWritePos > 0) {
            m_LineWritePos--;
            if (echo) {
                CHAR8 bs[] = "\b \b";
                this->Write((U8*)bs, 3);
            }
        }
        return;
    }

    if (m_LineWritePos >= LINE_BUFFER_SIZE - 1) {
        c = '\n'; // Force newline
    }

    // 5. Simpan ke Buffer Internal
    m_LineBuffer[m_LineWritePos++] = c;

    // 6. Echo ke layar
    if (echo) {
        this->Write((U8*)&c, 1);
    }
}

U32 TTY::Read(U8* buffer, U32 size){
    if (size == 0) return 0;

    // Reset posisi read lokal user
    U32 UserBytesCopied = 0;

    bool canonical = (m_Termios.c_lflag & ICANON);

    Tasking::Task* CurrentTask = Tasking::GetCurrentTaskPtr();

    // --- LOOP UTAMA ---
    while (UserBytesCopied < size) {

        if (CurrentTask->Signals != 0) {
            // Biasanya return -1 (Error) dengan errno = EINTR (Interrupted)
            // Tapi karena return type U32, kita return -1 (Max U32) 
            // nanti di syscall handler di cast ke int.
            return (U32)-1; 
        }

        bool DataReady = false;

        if (canonical) {
             // Cek apakah ada newline di buffer yang belum dibaca
             for(U32 i = m_LineReadPos; i < m_LineWritePos; i++){
                 if(m_LineBuffer[i] == '\n' || m_LineBuffer[i] == m_Termios.c_cc[VEOF]){
                     DataReady = true;
                     break;
                 }
             }
        } else {
             // Raw mode: ada karakter apapun, sikat.
             if(m_LineReadPos < m_LineWritePos) DataReady = true;
        }

        if (!DataReady) {
            // Belum ada data (atau belum di-Enter). 
            // Yield CPU biar task lain (seperti hexdump) bisa jalan.
            // JANGAN BLOCKING DI SINI TANPA YIELD!
            Tasking::SchedulerYield(); 
            continue;
        }

        // B. SALIN DATA (Sama kayak kodemu yg lama)
        while (m_LineReadPos < m_LineWritePos && UserBytesCopied < size) {
            char c = m_LineBuffer[m_LineReadPos++];
            buffer[UserBytesCopied++] = c;
            
            // Canonical mode break on newline
            if (canonical && (c == '\n' || c == m_Termios.c_cc[VEOF])) {
                return UserBytesCopied;
            }
        }
        
        // Reset buffer jika kosong (Circular buffer implementation lebih bagus sebenernya)
        if(m_LineReadPos == m_LineWritePos){
            m_LineReadPos = 0;
            m_LineWritePos = 0;
        }

        if (!canonical && m_LineReadPos >= m_LineWritePos) break;
    }

    return UserBytesCopied;
}

U32 TTY::Write(U8* Buffer, U32 Size){
    if(!Buffer || !Size) return -ROS_INVALID;

    // Cek flag output processing
    bool opost = (m_Termios.c_oflag & OPOST);
    bool onlcr = (m_Termios.c_oflag & ONLCR);

    U32 BytesWritten = 0;
    
    for(U32 i = 0; i < Size; i++) {
        CHAR8 c = (CHAR8)Buffer[i];

        // LOGIC ONLCR:
        // Kalau ketemu '\n', kita harus kirim "\r\n"
        if (opost && onlcr && c == '\n') {
            // FIX: Pakai Array 2 byte biar jadi Null-Terminated String
            CHAR8 cr[2] = {'\r', 0}; 
            
            if(FBConsole::IsReady()) FBConsole::WriteString(cr);
            Serial::Write(cr);
        }

        // Print karakter aslinya
        CHAR8 tmp[2] = {c, 0};
        
        if(FBConsole::IsReady()) FBConsole::WriteString(tmp);
        Serial::Write(tmp);
        
        BytesWritten++;
    }

    return BytesWritten;
}
INTN TTY::Ioctl(File* file, U32 command, U64 arg){
    (void)file; (void)arg;
    switch(command){
        case FIONREAD: {
            // Get number of bytes available to read
            U32* bytesAvailable = (U32*)(UPTR)arg;
            if(!bytesAvailable) return -ROS_INVALID;

            *bytesAvailable = PIC::Keyboard::GetBufferCount();
            return 0; // success
            break;
        }
        case TIOCGPGRP: {
            // Get Process Group ID (we use PID here)
            U64* pgid = (U64*)(UPTR)arg;
            if(!pgid) return -ROS_INVALID;

            *pgid = GetForegroundPID();
            return 0; // success
            break;
        }
        case TIOCSPGRP: {
            // Set Process Group ID (we use PID here)
            U64* pgid = (U64*)(UPTR)arg;
            if(!pgid) return -ROS_INVALID;

            Printk::Write(Printk::Level::LOG_INFO, "StdinDevice: Setting foreground PID to %llu\n", *pgid);
            SetForegroundPID(*pgid);
            return 0; // success
            break;
        }
        case TIOCGWINSZ: {
            // Get window size
            winsize* ws = (winsize*)(UPTR)arg;
            if(!ws) return -ROS_INVALID;

            ws->ws_row = FBConsole::GetRows();
            ws->ws_col = FBConsole::GetCols();
            ws->ws_xpixel = FBConsole::GetWidthPixels();
            ws->ws_ypixel = FBConsole::GetHeightPixels();
            return 0; // success
            break;
        }
        case TIOCSWINSZ: {
            // belom support karena kita masih belom bisa ganti FBConsole
            // struct
            winsize *ws = (winsize*)(UPTR)arg;
            if(!ws) return -ROS_INVALID;
            
            Printk::Write(Printk::Level::LOG_WARNING, "TTY Ioctl: TIOCSWINSZ not supported yet\n");
            return -ROS_UNSUPPORTED;
        }
        case TCGETS: {
            // Get termios settings
            termios* tio = (termios*)(UPTR)arg;
            if(!tio) return -ROS_INVALID;

            *tio = m_Termios;
            return 0; // success
            break;
        }
        case TCSETS: {
            // Set termios settings
            termios* tio = (termios*)(UPTR)arg;
            if(!tio) return -ROS_INVALID;

            m_Termios = *tio;
            return 0; // success
            break;
        }
        case TCSETSW:{
            // Set termios settings (wait for output drain)
            termios* tio = (termios*)(UPTR)arg;
            if(!tio) return -ROS_INVALID;

            // Since our TTY is immediate mode, no need to wait for drain
            m_Termios = *tio;
            return 0; // success
            break;
        }
        case TCSETSF:{
            // Set termios settings (flush input first)
            termios* tio = (termios*)(UPTR)arg;
            if(!tio) return -ROS_INVALID;

            // Flush input buffer
            PIC::Keyboard::FlushBuffer();

            m_Termios = *tio;
            return 0; // success
            break;
        }
        default:
            return -ROS_UNSUPPORTED;
    }
}

TTY::TTY(){
    CHAR8 TempNameTTY[32];
    CHAR8 TempNumTTY[20];

    // Build name safely: "tty" + index
    String::Strcpy(TempNameTTY, "tty");
    String::Itoa(StdDvc::TTYActive, TempNumTTY, 10);
    String::Strcat(TempNameTTY, TempNumTTY);

    // Copy into member buffer (size 32)
    String::Strcpy(m_name, TempNameTTY);
    StdDvc::TTYActive++;
    ForegroundPGID = 0;

    m_Termios.c_iflag = ICRNL;
    m_Termios.c_lflag = ICANON | ECHO | ISIG | ECHOE;
    m_Termios.c_oflag = OPOST | ONLCR;

    String::Memset(m_Termios.c_cc, 0, NCCS);
    m_Termios.c_cc[VINTR]  = 0x03; // Ctrl+C
    m_Termios.c_cc[VEOF]   = 0x04; // Ctrl+D
    m_Termios.c_cc[VERASE] = 0x7F; // Backspace (kadang 0x08 tergantung keyboard driver lo)
    m_Termios.c_cc[VKILL]  = 0x15; // Ctrl+U
    m_LineReadPos = 0;
    m_LineWritePos = 0;
    String::Memset(m_LineBuffer, 0, LINE_BUFFER_SIZE);
}

TTY::~TTY() {
    // nothing to free; name buffer is owned by object
}

RandomDevice::RandomDevice(const CHAR8* Name){
    m_Seed = Arch::ASM::RdTSC(); // initial seed
    m_name = Name ? Name : "random";
}

U32 RandomDevice::NextRand() {
    U32 Noise = EntrophySystem::GetSeed();

    m_Seed ^= Noise;
    
    U32 x = m_Seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    m_Seed = x;
    return x;
}

U32 RandomDevice::Read(U8* buffer, U32 size){
    if (size == 0) return 0;

    // Isi buffer user dengan angka acak byte-per-byte
    for(U32 i = 0; i < size; i++) {
        // Ambil byte paling bawah dari hasil random
        buffer[i] = (U8)(NextRand() & 0xFF);
    }
    
    return size;
}

U32 RandomDevice::Write(U8* buffer, U32 size){
    // Biasanya nulis ke /dev/random itu buat nambah entropy pool.
    // Tapi karena kita PRNG simple, kita mix aja input user ke seed
    // biar makin acak.
    for(U32 i = 0; i < size; i++) {
        m_Seed ^= buffer[i];
        NextRand(); // Shuffle dikit
    }
    return size;
}

namespace StdDvc{
    U64 TTYActive = 0;
    TTY *ListeningTTY;

    VOID RegisterSTD(DevFS* devfs){
        if(!devfs) return;

        TTY *tty = new TTY();

        // Register to DevFS
        devfs->RegisterCharDevice(tty, "tty");

        // Also register to DeviceManager for global lookup
        DeviceManager::RegisterCharDevice(tty);

        // set jadi default listening TTY
        ListeningTTY = tty;

        // utility STD selain stdin stdout stderr
        NullDevice *NullDvc = new NullDevice();
        RandomDevice *RandomDvc = new RandomDevice("random");
        RandomDevice *UrandomDvc = new RandomDevice("urandom");

        devfs->RegisterCharDevice(NullDvc, "null");
        devfs->RegisterCharDevice(RandomDvc, "random");
        devfs->RegisterCharDevice(UrandomDvc, "urandom");
        DeviceManager::RegisterCharDevice(NullDvc);
        DeviceManager::RegisterCharDevice(RandomDvc);
        DeviceManager::RegisterCharDevice(UrandomDvc);
    }
}
