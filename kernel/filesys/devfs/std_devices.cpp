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

U32 StdoutDevice::Write(U8 *Buffer, U32 Size){
    if(!Buffer || !Size) return -ROS_INVALID;

    const U32 CHUNK_SIZE = 128;
    CHAR8 Tmp[CHUNK_SIZE + 1];

    U32 BytesWritten = 0;

    while(BytesWritten < Size){
        U32 CopySize = Size - BytesWritten;
        if(CopySize > CHUNK_SIZE) CopySize = CHUNK_SIZE; // cap safety

        for(U32 i = 0; i < CopySize; i++){
            Tmp[i] = (CHAR8)Buffer[BytesWritten + i];
        }

        Tmp[CopySize] = '\0'; // null-terminate for Printk

        if(FBConsole::IsReady()){
            FBConsole::WriteString(Tmp);
        } 

        Serial::Write(Tmp);

        BytesWritten += CopySize;
    }

    return Size;
}

U32 StdoutDevice::Read(U8* buffer, U32 size){
    // harusnya gabisa baca STDOUT Device
    return -ROS_UNSUPPORTED;
}

INTN StdoutDevice::Ioctl(File* file, U32 command, U64 arg){
    (void)file; (void)arg;
    switch(command){
        case TIOCGWINSZ: {
            // Get window size
            StdDvc::winsize* ws = (StdDvc::winsize*)(UPTR)arg;
            if(!ws) return -ROS_INVALID;

            U32 cols = FBConsole::GetColumns();
            U32 rows = FBConsole::GetRows();

            ws->ws_col = (unsigned short)cols;
            ws->ws_row = (unsigned short)rows;
            ws->ws_xpixel = 0; // optional
            ws->ws_ypixel = 0; // optional

            return 0; // success
        }
        default:
            return -ROS_UNSUPPORTED;
    }
}

U32 StdinDevice::Read(U8* buffer, U32 size){
    if (size == 0) return 0;
    U32 count = 0;
    
    while (count < size) {
        // GetChar blocks if buffer is empty
        char c = PIC::Keyboard::GetChar();
        buffer[count++] = c;
        
        // Standard line-buffered behavior: return on newline
        if (c == '\n') break;
    }
    return count;
}

U32 StdinDevice::Write(U8* buffer, U32 size){
    // STDIN is Read-Only
    return -ROS_UNSUPPORTED;
}

INTN StdinDevice::Ioctl(File* file, U32 command, U64 arg){
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
        default:
            return -ROS_UNSUPPORTED;
    }
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
    VOID RegisterSTD(DevFS* devfs){
        if(!devfs) return;

        StdinDevice *Stdin = new StdinDevice();
        StdoutDevice *Stdout = new StdoutDevice();
        // Stderr uses same logic as Stdout for now
        StdoutDevice *Stderr = new StdoutDevice();

        // Register to DevFS
        devfs->RegisterCharDevice(Stdin, "stdin");
        devfs->RegisterCharDevice(Stdout, "stdout");
        devfs->RegisterCharDevice(Stderr, "stderr");

        // Also register to DeviceManager for global lookup
        DeviceManager::RegisterCharDevice(Stdin);
        DeviceManager::RegisterCharDevice(Stdout);
        DeviceManager::RegisterCharDevice(Stderr);

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
