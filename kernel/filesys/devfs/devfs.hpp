#pragma once
#include "../filesystem.hpp"
#include "../iblockdevice.hpp"

class ICharDevice{
    public:
        ICharDevice(){}
        virtual ~ICharDevice(){}

        virtual U32 Read(File *File, U8* buffer, U32 size) = 0;
        virtual U32 Write(File *File, U8* buffer, U32 size) = 0;
        virtual INTN Ioctl(File* file, U32 command, U64 arg) = 0;
        virtual short Poll(File* file, short events) = 0;

        virtual const CHAR8* GetDeviceName() = 0;
};

struct DevFile : public File{
    enum class DeviceType{
        NONE,
        BLOCK,
        CHAR
    };

    DeviceType Type;
    union{
        IBlockDevice* BlockDev;
        ICharDevice* CharDev;
    } dev;

    virtual short Poll(short events) override {
        // 1. Kalau ini Character Device (Keyboard, Serial, Mouse, TTY)
        if (Type == DeviceType::CHAR && dev.CharDev) {
            // Lempar tugasnya ke driver asli (TTY/Mouse)
            // 'this' dilempar sebagai pointer File*
            return dev.CharDev->Poll(this, events);
        }

        // 2. Kalau ini Block Device (Harddisk/Partition)
        else if (Type == DeviceType::BLOCK) {
            // Block device biasanya dianggap "Selalu Ready" 
            // kecuali lo implementasi async IO yang kompleks.
            short revents = 0;
            if (events & POLLIN)  revents |= POLLIN;
            if (events & POLLOUT) revents |= POLLOUT;
            return revents;
        }

        // 3. Tipe gak jelas / NONE
        return 0; // Gak ready apa-apa, atau return POLLNVAL
    }
};

class DevFS : public FileSystem{
    public:
        DevFS();
        virtual ~DevFS();

        virtual BOOL Mount(Partition *Part) override;
        virtual BOOL Unmount() override;

        virtual File* Open(const char* path, U32 Flags) override;
        virtual void Close(File* file) override;
        virtual U32 Read(File* file, U8* buffer, U32 size) override; 
        virtual U32 Write(File *File, U8 *Buffer, U32 Size) override;
        virtual BOOL Delete(const char* path) override;
        virtual BOOL Rename(const char* oldPath, const char* newPath) override;
        virtual BOOL Seek(File* file, U64 position, U32 Origin) override;
        virtual INTN Ioctl(File* file, U32 command, U64 arg) override;
        
        // Register/unregister device names local to this DevFS instance
        BOOL RegisterCharDevice(ICharDevice* dev, const CHAR8* name);
        BOOL RegisterBlockDevice(IBlockDevice* dev, const CHAR8* name);
        BOOL UnregisterDevice(const CHAR8* name);
        BOOL Init();
        
    // Additional FileSystem operations (no-op or not supported for devfs)
    virtual BOOL Truncate(File* file, U64 size) override;
    virtual BOOL MKDir(const char* path) override;
    virtual BOOL RMDir(const char* path) override;
    virtual BOOL Flush(File* file) override;
    virtual BOOL Append(File* file, U8* buffer, U32 size) override;
    virtual INTN ReadDir(File* dirFile, void* buffer, U32 bufferSize) override;
    virtual BOOL Stat(const char* path, FileInfo* info) override;
    
    private:
        struct DevEntry {
            BOOL Used;
            DevFile::DeviceType Type;
            CHAR8 Name[64];
            union {
                IBlockDevice* Block;
                ICharDevice* Char;
            } Ptr;
        };

        static const int MAX_ENTRIES = 32;
        DevEntry m_Entries[MAX_ENTRIES];
};

namespace DEVFS{
    BOOL Init();
    DevFS *GetInstanceToDevFS();
}