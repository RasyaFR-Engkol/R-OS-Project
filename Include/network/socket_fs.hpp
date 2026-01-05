#pragma once
#include <network/ethernet.hpp>
#include <filesystem/filesystem.hpp>
#include <network/tcp.hpp>
#include <network/udp.hpp>
#include <../kernel/driver/e1000/e1000.hpp>

struct SocketFile : public File{
    enum SockType { TCP, UDP, ICMP };
    SockType Type;

    union{
        Network::TCPSocket* Tcp;
        Network::UDPSocket* Udp;
        Network::ICMPSocket *Icmp;
    } Handle ;

    SocketFile() {
        RefCount = 1;
        IsDirectory = FALSE;
        FileSize = 0;
        CurrentPosition = 0;
    }

};

class SocketFileSystem : public FileSystem{
    public:
        static SocketFileSystem *Instance;
        static SocketFileSystem *GetInstance();
        U32 Read(File* file, U8* Buffer, U32 Size) override;

        U32 Write(File* file, U8* Buffer, U32 Size) override;

        void Close(File* file) override;

        BOOL Mount(Partition*) override { return TRUE; }
        File* Open(const char*, U32 Flags) override { return nullptr; }
        BOOL Delete(const char*) override { return FALSE; }
        BOOL Rename(const char*, const char*) override { return FALSE; }
        BOOL Seek(File*, U64, U32 o) override { return FALSE; }
        INTN Ioctl(File*, U32, U64) override { return 0; }
        BOOL Truncate(File*, U64) override { return FALSE; }
        BOOL MKDir(const char*) override { return FALSE; }
        BOOL RMDir(const char*) override { return FALSE; }
        BOOL Flush(File*) override { return TRUE; }
        BOOL Append(File*, U8*, U32) override { return FALSE; }
        INTN ReadDir(File*, void*, U32) override { return -1; }
        BOOL Stat(const char* path, FileInfo* info) override {return false;}
        BOOL Unmount() override {return false;}

};