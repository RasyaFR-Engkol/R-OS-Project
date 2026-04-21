#include <rossys.hpp>
#include <rosval.h>
#include <serial.hpp>
#include "../../driver/pic/pic.hpp"
#include "rng/entrophy.hpp"
#define PRINTK_MODULE_NAME "StdDvc"
#include <logging.hpp>
#include <string.hpp>
#include "std_devices.hpp"
#include "../../log/fbcon/fbcon.hpp"
#include "../../dev/devicemanager.hpp"
#include <task.hpp>

VOID TTY::OnInput(U8 c){
    Serial::Printf("TTY:'%c' (0x%02X)\n", c, c);

    if(c == 0x05){
        Tasking::Debug_DumpProcessState();
    }
    else if(c == 0x06){
        Tasking::Debug_MinorAndMajorFaultsBelowPID10();
    } else if(c == 0x07){
        Printk::Panic("Triggered panic VIA TTY.\n");
    } else if (c == 0x08){
        Tasking::DumpSchedulerTree();
    }

    if((m_Termios.c_iflag & IXON)){
        if(c == m_Termios.c_cc[VSTOP]){
            m_OutputStopped = TRUE;
            return;
        }
        if(c == m_Termios.c_cc[VSTART]){
            // TODO: Kalau lo punya Task yang lagi BLOCKED di TTY::Write, bangunin disini!
            return;
        }
    }

    if (c == 0) return;

    UNUSED__ bool canonical = (m_Termios.c_lflag & ICANON);
    bool echo      = (m_Termios.c_lflag & ECHO);
    bool mapCRNL   = (m_Termios.c_iflag & ICRNL);
    bool ignoreCR = (m_Termios.c_iflag & IGNCR);

    if(m_Termios.c_lflag & ISIG){
        INTN SignalToSend = 0;
        CHAR8 Visual = 0;

        // CTRL + C Handling
        if(c == m_Termios.c_cc[VINTR]){
            SignalToSend = 2;
            Visual = 'C';
        }
        else if(c == m_Termios.c_cc[VQUIT]){
            SignalToSend = 3;
            Visual = '\\';
        }

        if(SignalToSend != 0){
            U64 FGPid = GetForegroundPID();
            if(FGPid >= DEFAULT_CONFIG_PID_START){
                Tasking::SetTaskSignal(FGPid, SignalToSend, TRUE);

                if(echo){
                    CHAR8 Tmp[4] = {'^', Visual, '\n', 0,};
                    this->Write(NULL, (U8*)Tmp, 4);
                }

                m_LineWritePos = 0;
                m_LineReadPos = 0;
            }
            return;
        }
    }

    if (c == '\r' && ignoreCR) return;
    if (c == '\r' && mapCRNL && !ignoreCR) c = '\n';

    // Don't handle backspace here - let userland application handle it
    // Backspace will be passed through to the line buffer

    if (m_LineWritePos >= LINE_BUFFER_SIZE - 1) {
        return;
    }

    m_LineBuffer[m_LineWritePos++] = c;

    if (echo) {
        this->Write(nullptr, (U8*)&c, 1);

        if (c == '\n' && !echo && (m_Termios.c_lflag & ECHONL)) {
             this->Write(nullptr, (U8*)&c, 1);
        }
    }

    // --- WAKE UP LOGIC (Normal Input) ---
    BOOL Canonical = (m_Termios.c_lflag & ICANON);
    BOOL MustWake = !Canonical;

    if(Canonical){
        if(c == '\n' || c == m_Termios.c_cc[VEOF]){
            MustWake = TRUE;
        }
    }
    else {
        MustWake = TRUE;
    }

    if(MustWake){
        U64 fgPID = GetForegroundPID();
        Tasking::Task* task = Tasking::GetTaskPID(fgPID);
        
        if (task && task->State == Tasking::TaskState::BLOCKED) {
            task->State = Tasking::TaskState::READY;
            
            // Boost Priority biar interaktif
            task->vruntime = Tasking::MinVRuntime + 1;

            Tasking::CFSEnqueue(task);
            
            // Preemption
            Tasking::ForceReschedule = TRUE; 
        }
    }
}

U32 TTY::Read(File *file, U8* buffer, U32 size){
    if (size == 0) return 0;

    // Reset posisi read lokal user
    U32 UserBytesCopied = 0;
    bool canonical = (m_Termios.c_lflag & ICANON);
    Tasking::Task* CurrentTask = Tasking::GetCurrentTaskPtr();

    U32 MinCharsToRead = canonical ? 1 : m_Termios.c_cc[VMIN];
    if(MinCharsToRead == 0) MinCharsToRead = 1;
    // Note: VTIME (timeout) butuh timer OS, skip dulu buat skrg (anggap blocking forever).

    // --- LOOP UTAMA ---
    while (UserBytesCopied < size) {

        if (CurrentTask->Signals != 0) {
            return (U32)-1; 
        }

        bool DataReady = false;
        U32 CurrentAvail = 0;

        if (m_LineWritePos >= m_LineReadPos) {
            CurrentAvail = m_LineWritePos - m_LineReadPos;
        }

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
             if(CurrentAvail >= 1) DataReady = TRUE;
        }

        if (!DataReady) {
            Tasking::Task* current = Tasking::GetCurrentTaskPtr();
            if (current) {
                current->State = Tasking::TaskState::BLOCKED;
            }

            //Printk::Write(Printk::Level::LOG_INFO, "TTY Read: No data available, blocking task PID %llu\n", current ? current->pid : 0);
            Tasking::SchedulerYield(); 
            continue;
        }

        // B. SALIN DATA (Sama kayak kodemu yg lama)
        while (m_LineReadPos < m_LineWritePos && UserBytesCopied < size) {
            char c = m_LineBuffer[m_LineReadPos++];

            if (c == m_Termios.c_cc[VEOF]) {
                 if(m_LineReadPos == m_LineWritePos){
                    m_LineReadPos = 0; m_LineWritePos = 0;
                 }
                 return UserBytesCopied; 
            }

            buffer[UserBytesCopied++] = c;
            
            // Canonical mode break on newline
            if (canonical && c == '\n') {
                if(m_LineReadPos == m_LineWritePos){
                    m_LineReadPos = 0; m_LineWritePos = 0;
                }
                return UserBytesCopied;
            }
        }
        
        // if line read position equals line write position, reset both
        if(m_LineReadPos == m_LineWritePos){
            m_LineReadPos = 0;
            m_LineWritePos = 0;
        }

        // if no canonical mode and line read position is bigger or
        // equal to write position, break
        if (!canonical && m_LineReadPos >= m_LineWritePos) break;
    }

    return UserBytesCopied;
}

U32 TTY::Write(File *file, U8* Buffer, U32 Size){
    if(!Buffer || !Size) return -ROS_INVALID;
    (void)file;

    // Cek flag output processing
    bool opost = (m_Termios.c_oflag & OPOST);
    bool onlcr = (m_Termios.c_oflag & ONLCR);

    U32 BytesWritten = 0;

    while (m_OutputStopped) {
         // Cek signal biar bisa di-kill pas lagi stuck
        Tasking::Task* current = Tasking::GetCurrentTaskPtr();
        if (current->Signals != 0) return (U32)-1;

        current->State = Tasking::TaskState::BLOCKED;
        Tasking::SchedulerYield(); 
        // Pas bangun, cek lagi while(m_OutputStopped)...
    }
    
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
            
            Printk::Write(Printk::Level::LOG_DOK, "TTY Ioctl: TIOCSWINSZ not supported yet\n");
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

            m_Termios = *tio;
            return 0; // success
            break;
        }
        case TCSETSW:{
            // Set termios settings (wait for output drain)
            termios* tio = (termios*)(UPTR)arg;
            if(!tio) return -ROS_INVALID;

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
            Printk::Write(Printk::Level::LOG_DERR, "Unknown IOCTL fot TTY: %llu", command);
            return -ROS_UNSUPPORTED;
    }
}

short TTY::Poll(File *file, short events){
    short revents = 0;

    if(events & POLLIN){
        bool Canonical = (m_Termios.c_lflag & ICANON);

        if(Canonical){
            // --- CANONICAL MODE ---
            // Kita baru bilang "READY" kalau di buffer ada Newline (\n) atau EOF.
            // Persis sama kayak logic di TTY::Read lo.

            BOOL LineReady = FALSE;
            for(U32 i = m_LineReadPos; i < m_LineWritePos; i++){
                CHAR8 C = m_LineBuffer[i];
                if(C == '\n' || C == m_Termios.c_cc[VEOF]){
                    LineReady = TRUE;
                    break;
                }
            }

            if(LineReady){
                revents |= POLLIN;
            }
        } else {
            if (m_LineReadPos < m_LineWritePos) {
                revents |= POLLIN;
            }
        }
    }

    if(events & POLLOUT){
        revents |= POLLOUT;
    }

    return revents;
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
    ForegroundPGID = DEFAULT_CONFIG_PID_START;
    SetForegroundPID(ForegroundPGID);

    m_OutputStopped = FALSE;

    m_Termios.c_iflag = ICRNL | IXON;
    m_Termios.c_lflag = ICANON | ISIG | ECHOE | ECHO | IEXTEN;
    m_Termios.c_oflag = OPOST | ONLCR;

    String::Memset(m_Termios.c_cc, 0, NCCS);
    m_Termios.c_cc[VINTR]  = 0x03; // Ctrl+C
    m_Termios.c_cc[VQUIT]  = 0x1C; // Ctrl+\ (SIGQUIT) -> Fitur baru
    m_Termios.c_cc[VERASE] = 0x7F; // Backspace
    m_Termios.c_cc[VKILL]  = 0x15; // Ctrl+U
    m_Termios.c_cc[VEOF]   = 0x04; // Ctrl+D
    m_Termios.c_cc[VSTART] = 0x11; // Ctrl+Q (XON)  -> Fitur baru
    m_Termios.c_cc[VSTOP]  = 0x13; // Ctrl+S (XOFF) -> Fitur baru
    m_Termios.c_cc[VMIN]   = 1;    // Min 1 char (Standard blocking read)
    m_Termios.c_cc[VTIME]  = 0;    // No timeout

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

U32 RandomDevice::Read(File *file, U8* buffer, U32 size){
    if (size == 0) return 0;

    // Isi buffer user dengan angka acak byte-per-byte
    for(U32 i = 0; i < size; i++) {
        // Ambil byte paling bawah dari hasil random
        buffer[i] = (U8)(NextRand() & 0xFF);
    }
    
    return size;
}

U32 RandomDevice::Write(File* file, U8* buffer, U32 size){
    // Biasanya nulis ke /dev/random itu buat nambah entropy pool.
    // Tapi karena kita PRNG simple, kita mix aja input user ke seed
    // biar makin acak.
    (void)file;
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
