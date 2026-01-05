#pragma once
#include "rossys.hpp"
#include <rosval.h>
#include <spinlock/simple.hpp>
#include <../kernel/dev/devicemanager.hpp>
#include <../kernel/filesys/devfs/devfs.hpp>
#include <string.hpp>

struct MousePacket {
    U8 buttons;
    I8 dx;
    I8 dy;
};

class MouseDevice : public ICharDevice{
    private:
        static const INTN BUFFER_SIZE = 128; // Simpan 128 gerakan terakhir
        MousePacket m_Buffer[BUFFER_SIZE];
        INTN m_Head = 0;
        INTN m_Tail = 0;
        Tasking::WaitQueue m_WaitQueue;
        Arch::Spinlock::Spinlock m_Lock;

    public:
        MouseDevice();

        virtual short Poll(File* file, short events) override;
        virtual U32 Read(File *file, U8* buffer, U32 size) override;
        void Inject(I32 dx, I32 dy, U32 btn);
        virtual U32 Write(File* file, U8* buffer, U32 size) override { return 0; }
        virtual INTN Ioctl(File* file, U32 command, U64 arg) override { return -1; }

        virtual const CHAR8* GetDeviceName() override { return "mouse"; }
};

namespace MouseDriver {
    void Init(DevFS* devfs) ;
    void SendPacket(I32 dx, I32 dy, U32 btn);
}