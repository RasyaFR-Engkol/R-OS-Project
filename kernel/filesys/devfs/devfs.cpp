#include "devfs.hpp"
#include <rosval.h>
#include <filesystem/filesystem.hpp>
#include <mm.hpp>
#include <string.hpp>
#define PRINTK_MODULE_NAME "DevFS"
#include <logging.hpp>
#include "../../dev/devicemanager.hpp"
#include "../../task/reserved/inputdaemon/driver/mouse.hpp"

// Forward declaration for serial devfs registration helper implemented in
// kernel/driver/serial/serial_devfs_register.cpp
namespace SerialDriver { BOOL RegisterSerialToDevFS(class DevFS* devfs); }
namespace StdDvc { VOID RegisterSTD(class DevFS* devfs); }

struct NamedPipeEntry{
    CHAR8 Name[64];
    PipeBuffer *Buffer;
};
NamedPipeEntry NamedPipe[10]; // limit 10 aja dulu

void RemoveNamedPipeEntry(PipeBuffer* targetBuf) {
    for(INTN i = 0; i < 10; i++){
        if(NamedPipe[i].Buffer == targetBuf){
            // Reset entry biar bisa dipake lagi & ga dangling pointer
            NamedPipe[i].Buffer = nullptr;
            NamedPipe[i].Name[0] = '\0';
            return;
        }
    }
}

PipeBuffer *GetNamedPipeBuffer(const CHAR8* name){
    for(INTN i = 0; i < 10;i++){
        if(String::Strcmp(NamedPipe[i].Name, name) == 0){
            return NamedPipe[i].Buffer;
        }
    }
    return nullptr;
}

PipeBuffer *CreateNamedPipe(const CHAR8* Name){
    PipeBuffer *buf = new PipeBuffer();
    buf->RefCount = 0;

    for(INTN i = 0; i < 10; i++){
        if(!NamedPipe[i].Buffer){
            String::Strcpy(NamedPipe[i].Name, Name);
            buf->BytesAvailable = 0;
            buf->IsWriteClosed = FALSE;
            buf->WritePos = 0;
            buf->ReadPos = 0;
            buf->RefCount = 0;
            buf->Lock.Init();
            NamedPipe[i].Buffer = buf;
            Printk::Write(Printk::Level::LOG_INFO, "DevFS: Created named pipe '%s'\n", Name);
            return buf;
        }
    }
    Printk::Write(Printk::Level::LOG_ERR, "DevFS: Failed to create named pipe '%s', limit reached\n", Name);
    return nullptr; 
}

U64 DevFS::CreateNode(const char* path, U32 Flags) {
    if(!path || path[0] == '\0') {
        Printk::Write(Printk::Level::LOG_ERR, "DevFS: CreateNode called with invalid path\n");
        return 0;
    }

    // --- TAMBAHAN BARU: STRIP SLASH DI SINI ---
    const char* devName = (path[0] == '/') ? path + 1 : path;

    // Passing devName yang udah bersih ke PipeFS
    return PipeFileSystem::GetInstance()->CreateNode(devName, Flags);
}

U64 DevFS::Lookup(const char* path) {
    if (!path || path[0] == '\0') return 0;
    const char* devName = (path[0] == '/') ? path + 1 : path;

    PipeBuffer* pipe = GetNamedPipeBuffer(devName);
    if (pipe) {
        return (U64)(UPTR)pipe; // Ketemu pipanya, return InodeID 64-bit
    } 

    // Cari di registry
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (m_Entries[i].Used && String::Strcmp(m_Entries[i].Name, devName) == 0) {
            // Kita pakai alamat pointer device sebagai InodeID unik
            return (U64)(UPTR)m_Entries[i].Ptr.Char; 
        }
    }
    return 0; // Gak ketemu
}

BOOL DevFS::PopulateInode(U64 InodeID, ::Inode* vfsNode) {
    if (!vfsNode) return FALSE;

    for(INTN i = 0; i < 10; i++){
        if(NamedPipe[i].Buffer != nullptr && (U64)(UPTR)NamedPipe[i].Buffer == InodeID){
            vfsNode->Type = FT_PIPE;
            vfsNode->FSOwner = this; // Tetep DevFS ownernya di mata VFS
            vfsNode->FileSize = NamedPipe[i].Buffer->BytesAvailable;
            vfsNode->InodeID = InodeID;
            return TRUE;
        }
    }

    // Kita cari tahu ini InodeID punya siapa dengan scan registry lagi
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (m_Entries[i].Used && (U64)(UPTR)m_Entries[i].Ptr.Char == InodeID) {
            vfsNode->Type = (m_Entries[i].Type == DevFile::DeviceType::CHAR) ? FT_DEVCHAR : FT_DEVBLOK;
            vfsNode->FSOwner = this;
            vfsNode->FileSize = 0; 
            vfsNode->InodeID = InodeID;
            return TRUE;
        }
    }
    return FALSE;
}

BOOL DevFS::Mount(Partition *Part){
    (void)Part; // DevFS is pseudo-filesystem not backed by a partition
    return TRUE;
}

BOOL DevFS::Unmount(){
    return FALSE;
}

DevFS::DevFS(){
    // Initialize registry
    for(int i=0;i<MAX_ENTRIES;i++){
        m_Entries[i].Used = FALSE;
        m_Entries[i].Type = DevFile::DeviceType::NONE;
        m_Entries[i].Name[0] = '\0';
        m_Entries[i].Ptr.Block = nullptr;
    }
}

DevFS::~DevFS(){
    // Nothing to free: entries are only pointers to external device objects
}

BOOL DevFS::RegisterCharDevice(ICharDevice* dev, const CHAR8* name){
    if(!dev || !name) return FALSE;
    // avoid duplicates
    for(int i=0;i<MAX_ENTRIES;i++){
        if(m_Entries[i].Used && String::Strcmp((const CHAR8*)m_Entries[i].Name, name) == 0) return FALSE;
    }
    for(int i=0;i<MAX_ENTRIES;i++){
        if(!m_Entries[i].Used){
            m_Entries[i].Used = TRUE;
            m_Entries[i].Type = DevFile::DeviceType::CHAR;
            // copy name
            unsigned long long len = String::Strlen(name);
            unsigned long long tocpy = (len < sizeof(m_Entries[i].Name)-1) ? len : (sizeof(m_Entries[i].Name)-1);
            String::Memcpy(m_Entries[i].Name, name, tocpy);
            m_Entries[i].Name[tocpy] = '\0';
            m_Entries[i].Ptr.Char = dev;
            return TRUE;
        }
    }
    return FALSE; // registry full
}

BOOL DevFS::RegisterBlockDevice(IBlockDevice* dev, const CHAR8* name){
    if(!dev || !name) return FALSE;
    for(int i=0;i<MAX_ENTRIES;i++){
        if(m_Entries[i].Used && String::Strcmp((const CHAR8*)m_Entries[i].Name, name) == 0) return FALSE;
    }
    for(int i=0;i<MAX_ENTRIES;i++){
        if(!m_Entries[i].Used){
            m_Entries[i].Used = TRUE;
            m_Entries[i].Type = DevFile::DeviceType::BLOCK;
            unsigned long long len = String::Strlen(name);
            unsigned long long tocpy = (len < sizeof(m_Entries[i].Name)-1) ? len : (sizeof(m_Entries[i].Name)-1);
            String::Memcpy(m_Entries[i].Name, name, tocpy);
            m_Entries[i].Name[tocpy] = '\0';
            m_Entries[i].Ptr.Block = dev;
            return TRUE;
        }
    }
    return FALSE;
}

BOOL DevFS::UnregisterDevice(const CHAR8* name){
    if(!name) return FALSE;
    for(int i=0;i<MAX_ENTRIES;i++){
        if(m_Entries[i].Used && String::Strcmp((const CHAR8*)m_Entries[i].Name, name) == 0){
            m_Entries[i].Used = FALSE;
            m_Entries[i].Type = DevFile::DeviceType::NONE;
            m_Entries[i].Name[0] = '\0';
            m_Entries[i].Ptr.Block = nullptr;
            return TRUE;
        }
    }
    return FALSE;
}

void DevFS::Close(File* file) {
    // KOSONGIN AJA! 
    // Biar VFSManager (Si Mandor) yang bersih-bersih memori Node dan File.
    return;
}

U32 DevFS::Read(File* file, U8* buffer, U32 size){
    //Serial::Printf("DevFS: Read called for file=%p, buffer=%p, size=%u\n", file, buffer, size);
    // Pastiin file->Node aman biar gak null pointer
    if(!file || !buffer || size == 0 || !file->Node) return 0;

    if(file->Node->Type == FT_DEVCHAR){
        // Tarik pointer ICharDevice dari InodeID (sesuai fungsi Lookup/Stat lu)
        ICharDevice* cdev = (ICharDevice*)(UPTR)file->Node->InodeID;
        //Serial::Printf("Reading from device: %s\n", cdev->GetDeviceName());
        if(cdev) return cdev->Read(file, buffer, size);
        return 0;

    } else if(file->Node->Type == FT_DEVBLOK){
        // Tarik pointer IBlockDevice dari InodeID
        IBlockDevice* bdev = (IBlockDevice*)(UPTR)file->Node->InodeID;
        if(!bdev) return 0;

        // Logika DMA lu yang udah solid kemaren.
        // PENTING: Pake file->CurrentPosition, bukan df->CurrentPosition lagi.
        const U32 SECTOR = 512;
        U64 startByte = file->CurrentPosition; 
        U64 endByte = startByte + (U64)size;
        U64 firstLBA = startByte / SECTOR;
        U32 offset = (U32)(startByte % SECTOR);
        U64 lastLBA = (endByte == 0) ? firstLBA : (endByte - 1) / SECTOR;
        U32 count = (U32)(lastLBA - firstLBA + 1);

        PageAlloc::DMAAlloc::DMABuffer *buf = nullptr;
        
        // Panggil ReadSectors langsung dari bdev
        if(!bdev->ReadSectors(firstLBA, count, &buf)) return 0;

        SIZE_T available = (SIZE_T)count * SECTOR;
        if (offset >= (U32)available) {
            PageAlloc::DMAAlloc::FreeDMABuffer(buf);
            return 0;
        }
        
        U32 maxCopy = (U32)(available - offset);
        U32 toCopy = (size > maxCopy) ? maxCopy : size;

        U8 *src = (U8*)(UPTR)buf->VirtAddr + offset;
        String::Memcpy(buffer, src, toCopy);

        PageAlloc::DMAAlloc::FreeDMABuffer(buf);

        // Update posisi
        file->CurrentPosition += toCopy;
        return toCopy;
    } else if (file->Node->Type == FT_PIPE) {
        return PipeFileSystem::GetInstance()->Read(file, buffer, size);
    }
    
    return 0;
}

U32 DevFS::Write(File *File, U8 *Buffer, U32 Size){
    //Serial::Printf("DevFS: Write called for file=%p, buffer=%p, size=%u\n", File, Buffer, Size);
    // Pastiin File dan Node aman!
    if(!File || !Buffer || Size == 0 || !File->Node) return 0;

    // 1. Karakter Device
    if(File->Node->Type == FT_DEVCHAR){
        ICharDevice* cdev = (ICharDevice*)(UPTR)File->Node->InodeID;
        //Serial::Printf("Reading from device: %s\n", cdev->GetDeviceName());
        if(cdev) return cdev->Write(File, Buffer, Size);
        return 0;
    } 
    // 2. Block Device
    else if(File->Node->Type == FT_DEVBLOK){
        IBlockDevice* bdev = (IBlockDevice*)(UPTR)File->Node->InodeID;
        if(!bdev) return 0;

        const U32 SECTOR = 512;
        // Pake File->CurrentPosition (bukan df-> lagi)
        U64 startByte = File->CurrentPosition;
        U64 endByte = startByte + (U64)Size;
        U64 firstLBA = startByte / SECTOR;
        U32 offset = (U32)(startByte % SECTOR);
        U64 lastLBA = (endByte == 0) ? firstLBA : (endByte - 1) / SECTOR;
        U32 count = (U32)(lastLBA - firstLBA + 1);

        SIZE_T bytesNeeded = (SIZE_T)count * SECTOR;
        PageAlloc::DMAAlloc::DMABuffer *buf = nullptr;

        if (offset == 0 && (SIZE_T)Size >= bytesNeeded) {
            // Full-sector write
            buf = PageAlloc::DMAAlloc::AllocateDMABytes(bytesNeeded);
            if (!buf) return 0;
            U8 *dst = (U8*)(UPTR)buf->VirtAddr;
            U32 toCopy = (Size > (U32)bytesNeeded) ? (U32)bytesNeeded : Size;
            String::Memcpy(dst, Buffer, toCopy);
            
            if ((SIZE_T)toCopy < bytesNeeded) {
                String::Memset(dst + toCopy, 0, bytesNeeded - toCopy);
            }
        } else {
            // Partial-sector write
            // Panggil bdev langsung (bukan df->dev.BlockDev)
            if(!bdev->ReadSectors(firstLBA, count, &buf)) return 0;
            U8 *dst = (U8*)(UPTR)buf->VirtAddr + offset;
            String::Memcpy(dst, Buffer, Size);
        }

        // Panggil bdev langsung
        BOOL ok = bdev->WriteSectors(firstLBA, count, buf);
        PageAlloc::DMAAlloc::FreeDMABuffer(buf);
        if(!ok) return 0;

        // Update posisi
        File->CurrentPosition += Size;
        return Size;
    } else if (File->Node->Type == FT_PIPE) {
        return PipeFileSystem::GetInstance()->Write(File, Buffer, Size);
    }
    
    return 0;
}

INTN DevFS::Ioctl(File* file, U32 command, U64 arg){
    if(!file || !file->Node) return -1;

    // Cuma Character Device yang biasanya nerima Ioctl (kayak set baud rate Serial, dll)
    if(file->Node->Type == FT_DEVCHAR){
        ICharDevice* cdev = (ICharDevice*)(UPTR)file->Node->InodeID;
        if(cdev) return cdev->Ioctl(file, command, arg);
        return -1;
    }
    return -1;
}

BOOL DevFS::Delete(const char* path){
    (void)path; return FALSE; // Not supported
}

BOOL DevFS::Rename(const char* oldPath, const char* newPath){
    (void)oldPath; (void)newPath; return FALSE; // Not supported
}

BOOL DevFS::Seek(File* file, U64 position, U32 Origin){
    // NANTI HITUNG ORIGINNYA. KALO DARI TENGAH YA TENGAH, AKHIR YA
    // AKHIR, AWAL YA AWAL
    if(!file) return FALSE;
    file->CurrentPosition = (U64)position;
    return TRUE;
}

BOOL DevFS::Truncate(File* file, U64 size){
    (void)file; (void)size; // Not supported for devfs
    return FALSE;
}

BOOL DevFS::MKDir(const char* path){
    (void)path; return FALSE; // Not supported
}

BOOL DevFS::RMDir(const char* path){
    (void)path; return FALSE; // Not supported
}

BOOL DevFS::Flush(File* file){
    (void)file; return TRUE; // Nothing to flush for devfs
}

BOOL DevFS::Append(File* file, U8* buffer, U32 size){
    // Default to writing at current position
    if(!file || !buffer || size == 0) return FALSE;
    
    // Ganti Read jadi Write!
    U32 written = Write(file, buffer, size); 
    
    return (written > 0);
}

INTN DevFS::ReadDir(File* dirFile, void* buffer, U32 bufferSize){
    if(!dirFile || !buffer) return -1;
    if(!dirFile->IsDirectory) return -1;
    if(bufferSize == 0) return 0;

    char* out = (char*)buffer;
    U32 remain = bufferSize;

    // Use CurrentPosition as number of entries already returned
    U64 skip = dirFile->CurrentPosition;
    U64 seen = 0;

    // First, list local registry entries
    for(int i=0;i<MAX_ENTRIES;i++){
        if(!m_Entries[i].Used) continue;
        const CHAR8* name = m_Entries[i].Name;
        // count this entry
        U64 idx = seen++;
        if(idx < skip) continue;
        U32 need = (U32)(String::Strlen(name) + 1);
        if(need > remain) { seen--; break; }
        String::Memcpy((U8*)out, (const U8*)name, need);
        out += need; remain -= need;
    }

    // Debug: how many entries total were reported
    //Printk::Write(Printk::Level::LOG_INFO, "DevFS: ReadDir total reported=%llu\n", (unsigned long long)seen);

    // update position to number of entries seen
    dirFile->CurrentPosition = seen;

    U32 used = bufferSize - remain;
    return (INTN)used;
}

BOOL DevFS::Stat(const char *path, FileInfo *Info){
    if(!path || !Info) return FALSE;

    // 1. Handle Root Directory
    // Path dari VFS biasanya absolut relatif terhadap mount point.
    // Bisa "/" atau "" tergantung implementasi VFS lo.
    if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
        String::Memset(Info, 0, sizeof(FileInfo));
        Info->Type = FileType::FT_DIR;
        Info->IsDirectory = TRUE;
        Info->Size = 0;
        Info->InodeID = 1; // Arbitrary ID untuk root devfs
        return TRUE;
    }

    // Skip leading slash (misal "/tty" jadi "tty")
    const char* devName = path;
    if (devName[0] == '/') devName++;

    // 2. Cek Named Pipes
    // (Ini penting biar nggak dianggap file biasa)
    PipeBuffer* pipe = GetNamedPipeBuffer(devName);
    if (pipe) {
        String::Memset(Info, 0, sizeof(FileInfo));
        Info->Type = FileType::FT_PIPE;
        Info->IsDirectory = FALSE;
        Info->Size = pipe->BytesAvailable; // Opsional: kasih tau ada berapa byte
        Info->InodeID = (U64)pipe; // Gunakan alamat memori sbg InodeID (biar unik)
        return TRUE;
    }

    // 3. Cek Internal Registry (m_Entries)
    // Ini buat device yang diregister manual kayak FB, Serial, Mouse
    for (int i = 0; i < MAX_ENTRIES; ++i) {
        if (!m_Entries[i].Used) continue;
        
        if (String::Strcmp((const CHAR8*)m_Entries[i].Name, (const CHAR8*)devName) == 0) {
            String::Memset(Info, 0, sizeof(FileInfo));
            Info->IsDirectory = FALSE;
            
            if (m_Entries[i].Type == DevFile::DeviceType::CHAR) {
                Info->Type = FileType::FT_DEVCHAR;
                Info->InodeID = (U64)m_Entries[i].Ptr.Char;
            } else if (m_Entries[i].Type == DevFile::DeviceType::BLOCK) {
                Info->Type = FileType::FT_DEVBLOK;
                Info->InodeID = (U64)m_Entries[i].Ptr.Block;
            } else {
                Info->Type = FileType::FT_NORMAL; // Fallback
            }
            
            Info->Size = 0; 
            return TRUE;
        }
    }

    // 4. Cek Device Manager (Block Device)
    // Misal: hda, sda
    IBlockDevice* bdev = DeviceManager::FindBlockDevice(devName);
    if (bdev) {
         String::Memset(Info, 0, sizeof(FileInfo));
         Info->Type = FileType::FT_DEVBLOK;
         Info->IsDirectory = FALSE;
         Info->InodeID = (U64)bdev;
         // Kalau lo punya fungsi GetTotalSectors(), bisa dikali 512 buat dapet Size
         Info->Size = 0; 
         return TRUE;
    }

    // 5. Cek Device Manager (Char Device)
    ICharDevice* cdev = DeviceManager::FindCharDevice(devName);
    if (cdev) {
         String::Memset(Info, 0, sizeof(FileInfo));
         Info->Type = FileType::FT_DEVCHAR;
         Info->IsDirectory = FALSE;
         Info->InodeID = (U64)cdev;
         Info->Size = 0;
         return TRUE;
    }

    // 6. Gak ketemu
    return FALSE;
}

short DevFS::Poll(File* file, short events) {
    if (!file || !file->Node) return 0;

    // Tarik pointer ICharDevice dari Inode. 
    // Sesuaikan sama cara lu nyimpen pointernya pas DevFS::CreateNode / Lookup!
    if(file->Node->Type == FT_DEVCHAR){
        ICharDevice* cdev = (ICharDevice*)(UPTR)file->Node->InodeID;
        if(cdev) return cdev->Poll(file, events);
        return 0;
    } else if (file->Node->Type == FT_PIPE) {
        return PipeFileSystem::GetInstance()->Poll(file, events);
    }
    
    return 0;
}

namespace FBDriver{
    BOOL RegisterFBToDevFS(DevFS* devfs);
}

namespace DEVFS{
    DevFS *g_DevFS = nullptr;
    BOOL Init(){
        DevFS *devfs = new DevFS();
        g_DevFS = devfs;
        if (VFSManager::MountFS("/dev", devfs)) {
            // Beberapa yang harus di register ke devfs
            FBDriver::RegisterFBToDevFS(devfs);
            SerialDriver::RegisterSerialToDevFS(devfs);
            StdDvc::RegisterSTD(devfs);
            MouseDriver::Init(devfs);
            return TRUE;
        }

        return FALSE;
    }

    DevFS *GetInstanceToDevFS(){
        if(g_DevFS) return g_DevFS;
        return nullptr;
    }
}