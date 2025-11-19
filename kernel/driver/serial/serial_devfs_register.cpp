#include "../../filesys/devfs/devfs.hpp"
#include <logging.hpp>
#include <string.hpp>
#include "../../../Include/serial.hpp"
#include "../../dev/devicemanager.hpp"

// ICharDevice wrapper for the COM1 serial port (ttyS0)
class SerialCharDevice : public ICharDevice {
public:
    SerialCharDevice(const CHAR8* name){
        unsigned long long len = String::Strlen(name);
        unsigned long long tocpy = (len < sizeof(m_Name)-1) ? len : (sizeof(m_Name)-1);
        String::Memcpy(m_Name, name, tocpy);
        m_Name[tocpy] = '\0';
    }

    virtual ~SerialCharDevice(){}

    virtual U32 Read(U8* buffer, U32 size) override {
        if(!buffer || size == 0) return 0;
        U32 count = 0;
        char ch;
        // Non-blocking read: consume up to 'size' bytes if available
        while (count < size) {
            if (Serial::TryReadChar(&ch)) {
                buffer[count++] = (U8)ch;
            } else break;
        }
        return count;
    }

    virtual U32 Write(U8* buffer, U32 size) override {
        if(!buffer || size == 0) return 0;
        // Write each byte to serial port
        for(U32 i=0;i<size;i++) {
            Serial::SerialPutC((char)buffer[i]);
        }
        return size;
    }

    virtual const CHAR8* GetDeviceName() override {
        return m_Name;
    }

private:
    CHAR8 m_Name[64];
};

namespace SerialDriver {
    BOOL RegisterSerialToDevFS(DevFS* devfs){
        static SerialCharDevice* inst = nullptr;
        if(inst) return TRUE;

        inst = new SerialCharDevice((const CHAR8*)"ttyS0");
        if(!inst) return FALSE;

        BOOL ok1 = FALSE, ok2 = FALSE;
        if(devfs) ok1 = devfs->RegisterCharDevice(inst, (const CHAR8*)"ttyS0");
        ok2 = DeviceManager::RegisterCharDevice(inst);

        Printk::Write(Printk::Level::LOG_INFO, "SerialDev: Registered ttyS0 (devfs=%d, devmgr=%d)\n", ok1 ? 1 : 0, ok2 ? 1 : 0);
        return TRUE;
    }
}
