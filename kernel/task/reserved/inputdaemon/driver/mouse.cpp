#include <rosval.h>
#include "mouse.hpp"

static MouseDevice* g_MouseDevice = nullptr;

namespace MouseDriver {
    void Init(DevFS* devfs) {
        if (g_MouseDevice) return;
        g_MouseDevice = new MouseDevice();
        
        // Register ke /dev/mouse
        if(devfs) devfs->RegisterCharDevice(g_MouseDevice, "mouse");
        DeviceManager::RegisterCharDevice(g_MouseDevice);
    }

    // Helper buat InputDaemon
    void SendPacket(I32 dx, I32 dy, U32 btn) {
        if(g_MouseDevice) g_MouseDevice->Inject(dx, dy, btn);
    }
}

MouseDevice::MouseDevice(){
    m_Head = 0;
    m_Tail = 0;
    m_Lock.Init();
}

short MouseDevice::Poll(File* file, short events){
    short revents = 0;
    if (events & POLLIN) {
        if (m_Head != m_Tail) revents |= POLLIN; // Ada data buat dibaca
    }
    return revents;
}

U32 MouseDevice::Read(File *f, U8 *buf, U32 size){
    // 1. Cek ukuran buffer user cukup gak buat nampung 3 integer (12 bytes)
    // Kita asumsikan user struct-nya { int dx; int dy; uint btn; }
    U32 RequiredSize = sizeof(I32) * 3; 
    if (size < RequiredSize) return 0;

    // Gausah sleep pake CLI STI goblog. kan udah mati dari interrupt MSI nya
    m_Lock.Acquire(); 
    
    while (m_Head == m_Tail) {
        m_Lock.Release(); 
        Tasking::SleepOn(m_WaitQueue); 
        m_Lock.Acquire(); 
    }

    // Ambil data dari Ring Buffer Kernel
    // Asumsi m_Buffer isinya struct packet kecil (I8 dx, I8 dy, U8 btn)
    auto kernelPkt = m_Buffer[m_Tail]; 
    m_Tail = (m_Tail + 1) % BUFFER_SIZE;
    
    m_Lock.Release();

    // --- PERBAIKAN DI SINI ---
    // Kita casting buffer user jadi pointer integer biar gampang nulis 4 byte
    I32* UserBufferAsInt = (I32*)buf;

    // Masukin data secara berurutan sesuai struct di Compositor
    // struct MousePacket { int dX; int dY; unsigned int Buttons; };
    
    // Perhatikan urutan assign-nya harus sama persis sama struct user!
    UserBufferAsInt[0] = (I32)kernelPkt.dx;      // Expand I8 ke I32 (otomatis handle minus)
    UserBufferAsInt[1] = (I32)kernelPkt.dy;      // Expand I8 ke I32
    UserBufferAsInt[2] = (I32)kernelPkt.buttons; // Expand U8 ke U32
    
    // Return jumlah byte total (12 bytes)
    return RequiredSize; 
}

VOID MouseDevice::Inject(I32 dx, I32 dy, U32 btn){
    m_Lock.Acquire();
            
            // Masukkan ke ring buffer
            INTN next = (m_Head + 1) % BUFFER_SIZE;
            if (next != m_Tail) { // Jika buffer tidak penuh
                m_Buffer[m_Head].buttons = (U8)btn;
                m_Buffer[m_Head].dx = (I8)dx;
                m_Buffer[m_Head].dy = (I8)dy;
                m_Head = next;
            }

            m_Lock.Release();

            // BANGUNKAN semua yang lagi nunggu data di /dev/mouse
            Tasking::WakeUp(m_WaitQueue); 
}