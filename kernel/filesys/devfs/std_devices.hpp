#pragma once

#include <rosval.h>
#include "devfs.hpp"
#include <task.hpp>

//STDIN
#define FIONREAD 0x541B // Get bytes available in input buffer
#define TIOCSPGRP 0x5410 // Set Process Group (Kita pake buat Set PID dulu)
#define TIOCGPGRP 0x540F // Get Process Group

//STDOUT
#define TIOCGWINSZ 0x5413 // Command ID standar Linux

// Use the foreground PID from the Tasking namespace
// (declared in `kernel/task/task.hpp`)

class StdinDevice : public ICharDevice {
    private:
        U64 ForegroundPGID;

    public:
        StdinDevice(){}
        virtual ~StdinDevice(){}

        virtual U32 Read(U8* buffer, U32 size) override; 
        virtual U32 Write(U8* buffer, U32 size) override;
        virtual const CHAR8* GetDeviceName() override { return "stdin"; }
        virtual INTN Ioctl(File* file, U32 command, U64 arg) override;

        VOID SetForegroundPID(U64 pid) {
            Tasking::g_ForegroundPID = pid;
        }

        U64 GetForegroundPID() {
            return Tasking::g_ForegroundPID;
        }
};

class StdoutDevice : public ICharDevice {
    public:
        StdoutDevice(){}
        virtual ~StdoutDevice(){}

        virtual U32 Read(U8* buffer, U32 size) override; 
        virtual U32 Write(U8* buffer, U32 size) override;
        virtual const CHAR8* GetDeviceName() override { return "stdout"; }
        virtual INTN Ioctl(File* file, U32 command, U64 arg) override;
};

class NullDevice : public ICharDevice {
    public:
        NullDevice(){}
        virtual ~NullDevice(){}

        virtual U32 Read(U8* buffer, U32 size) override {
            (void)buffer; (void)size; return 0; // EOF
        }
        virtual U32 Write(U8* buffer, U32 size) override {
            (void)buffer; return size; // discard data
        }
        virtual const CHAR8* GetDeviceName() override { return "null"; }
        virtual INTN Ioctl(File* file, U32 command, U64 arg) override {
            (void)file; (void)command; (void)arg; return -ROS_UNSUPPORTED;
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

        virtual U32 Read(U8* buffer, U32 size) override;
        virtual U32 Write(U8* buffer, U32 size) override;
        virtual const CHAR8* GetDeviceName() override { return m_name; }
        virtual INTN Ioctl(File* file, U32 command, U64 arg) override {
            (void)file; (void)command; (void)arg; return -ROS_UNSUPPORTED;
        }
};

namespace StdDvc{
    VOID RegisterSTD(DevFS* devfs);

    struct PACKSTRUCT winsize {
        unsigned short ws_row;    // rows, in characters
        unsigned short ws_col;    // columns, in characters
        unsigned short ws_xpixel; // horizontal size, pixels (optional)
        unsigned short ws_ypixel; // vertical size, pixels (optional)
    };
}