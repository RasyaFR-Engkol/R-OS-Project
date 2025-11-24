#include "../../filesys/devfs/devfs.hpp"
#include <logging.hpp>
#include <string.hpp>
#include "../../log/fbcon/fbcon.hpp"
#include "../../dev/devicemanager.hpp"

// Simple ICharDevice wrapper around FBConsole so we can register it with DevFS
class FBCharDevice : public ICharDevice {
public:
    FBCharDevice(const CHAR8* name){
        unsigned long long len = String::Strlen(name);
        unsigned long long tocpy = (len < sizeof(m_Name)-1) ? len : (sizeof(m_Name)-1);
        String::Memcpy(m_Name, name, tocpy);
        m_Name[tocpy] = '\0';
    }

    virtual ~FBCharDevice(){}

    virtual U32 Read(U8* buffer, U32 size) override {
        (void)buffer; (void)size; return 0; // no input from framebuffer
    }

    virtual U32 Write(U8* buffer, U32 size) override {
        if(!buffer || size == 0) return 0;
        // make temporary null-terminated string
        CHAR8 *tmp = new CHAR8[size + 1];
        if(!tmp) return 0;
        for(U32 i=0;i<size;i++) tmp[i] = (CHAR8)buffer[i];
        tmp[size] = '\0';
        if(FBConsole::IsReady()) FBConsole::WriteString(tmp);
        delete[] tmp;
        return size;
    }

    virtual const CHAR8* GetDeviceName() override {
        return m_Name;
    }

    virtual INTN Ioctl(File* file, U32 command, U64 arg) override {
        (void)file; (void)command; (void)arg; return -ROS_UNSUPPORTED;
    }

private:
    CHAR8 m_Name[64];
};

namespace FBDriver{
    // Public helper: register a /dev/ttyfb0 entry into the provided DevFS.
    // If devfs is nullptr, this will try to register with DeviceManager only.
    BOOL RegisterFBToDevFS(DevFS* devfs){
        static FBCharDevice* inst = nullptr;
        if(inst) return TRUE; // already registered

        inst = new FBCharDevice((const CHAR8*)"ttyfb0");
        if(!inst) return FALSE;

        BOOL ok1 = FALSE, ok2 = FALSE;
        if(devfs) ok1 = devfs->RegisterCharDevice(inst, (const CHAR8*)"ttyfb0");
        ok2 = DeviceManager::RegisterCharDevice(inst);

        Printk::Write(Printk::Level::LOG_INFO, "FBDev: Registered ttyfb0 (devfs=%d, devmgr=%d)\n", ok1 ? 1 : 0, ok2 ? 1 : 0);
        return TRUE;
    }
}