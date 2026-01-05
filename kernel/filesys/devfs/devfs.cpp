#include "devfs.hpp"
#include <rosval.h>
#include <filesystem/filesystem.hpp>
#include <mm.hpp>
#include <string.hpp>
#define PRINTK_MODULE_NAME "DevFS"
#include <logging.hpp>
#include "../../dev/devicemanager.hpp"
#include "../../driver/fb/fbcon_driver.hpp"
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
            return buf;
        }
    }
    return nullptr; 
}

File* DevFS::Open(const char* path, U32 Flags) {
    // Path dari VFS akan relatif, e.g. "/hda"
    // Kita asumsikan VFSManager::ResolvePath memberi kita path seperti "/hda"
    // (Seperti di vfs.cpp Anda, path rel akan diawali '/')

    if (!path || path[0] != '/') return nullptr;
    const char* devName = path + 1; // Lewati '/'

    PipeBuffer *TargetBuf = GetNamedPipeBuffer(devName);

    if(!TargetBuf && (Flags & O_CREAT)){
        TargetBuf = CreateNamedPipe(devName);
        if(!TargetBuf) return nullptr;
    }

    else if(TargetBuf && (Flags & O_CREAT) && (Flags & O_EXCL)){
        return nullptr;;
    }

    if(TargetBuf){
        BOOL IsWriteMode = (Flags & O_WRONLY) || (Flags & O_RDWR);

        PipeFile *PF = new PipeFile(TargetBuf, IsWriteMode);
        PF->FSOwner = PipeFileSystem::GetInstance();
        PF->RefCount = 1;
        PF->type = FileType::FT_PIPE;
        PF->Flags = Flags;
        String::Strcpy(PF->FileName, devName);

        TargetBuf->RefCount++;

        return PF;
    }

    if (devName[0] == '\0') {
        DevFile* f = new DevFile();
        f->CurrentPosition = 0;
        f->IsDirectory = TRUE; // Tandai sebagai direktori
        f->FileSize = 0;
        String::Strcpy(f->FileName, "/"); // atau "dev"
        f->FSOwner = this;
        f->Type = DevFile::DeviceType::NONE; // Bukan device
        f->dev.BlockDev = nullptr;
        f->RefCount = 1; // Init RefCount
        return f;
    }

    // First: check DevFS local registry for an exact name match
    for (int i = 0; i < MAX_ENTRIES; ++i) {
        if (!m_Entries[i].Used) continue;
        if (String::Strcmp((const CHAR8*)m_Entries[i].Name, (const CHAR8*)devName) == 0) {
            // Create handle depending on type
            if (m_Entries[i].Type == DevFile::DeviceType::CHAR) {
                DevFile* f = new DevFile();
                f->CurrentPosition = 0;
                f->IsDirectory = FALSE;
                String::Strcpy(f->FileName, devName);
                f->FSOwner = this;
                f->Type = DevFile::DeviceType::CHAR;
                f->dev.CharDev = m_Entries[i].Ptr.Char;
                f->FileSize = 0;
                f->RefCount = 1; // Init RefCount
                f->type = FileType::FT_DEVCHAR;
                return f;
            } else if (m_Entries[i].Type == DevFile::DeviceType::BLOCK) {
                DevFile* f = new DevFile();
                f->CurrentPosition = 0;
                f->IsDirectory = FALSE;
                String::Strcpy(f->FileName, devName);
                f->FSOwner = this;
                f->Type = DevFile::DeviceType::BLOCK;
                f->dev.BlockDev = m_Entries[i].Ptr.Block;
                f->FileSize = 0;
                f->RefCount = 1; // Init RefCount
                f->type = FileType::FT_DEVBLOK;
                return f;
            }
        }
    }

    // 1. Coba cari sebagai block device
    IBlockDevice* bdev = DeviceManager::FindBlockDevice(devName);
    if (bdev) {
        DevFile* f = new DevFile();
        f->CurrentPosition = 0;
        f->IsDirectory = FALSE;
        String::Strcpy(f->FileName, devName);
        f->FSOwner = this;
        f->Type = DevFile::DeviceType::BLOCK;
        f->dev.BlockDev = bdev;
        // f->FileSize = bdev->GetSectorCount() * 512; // (Jika Anda menambahkan GetSectorCount ke IBlockDevice)
        f->FileSize = 0;
        f->RefCount = 1; // Init RefCount
        f->type = FileType::FT_DEVBLOK;
        f->Flags = Flags;
        return f;
    }

    // 2. Jika tidak ketemu, coba cari sebagai char device
    ICharDevice* cdev = DeviceManager::FindCharDevice(devName);
    if (cdev) {
        DevFile* f = new DevFile();
        f->CurrentPosition = 0;
        f->IsDirectory = FALSE;
        String::Strcpy(f->FileName, devName);
        f->FSOwner = this;
        f->Type = DevFile::DeviceType::CHAR;
        f->dev.CharDev = cdev;
        f->FileSize = 0; // Char device tidak punya ukuran
        f->RefCount = 1; // Init RefCount
        f->type = FileType::FT_DEVCHAR;
        f->Flags = Flags;
        return f;
    }
    
    // 3. Jika tidak ketemu keduanya
    return nullptr; // Device tidak ditemukan
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

void DevFS::Close(File* file){
    if(!file) return;
    // Files returned by DevFS are allocated with new DevFile()
    delete file;
}

U32 DevFS::Read(File* file, U8* buffer, U32 size){
    if(!file || !buffer || size == 0) return 0;
    DevFile* df = static_cast<DevFile*>(file);

    if(df->Type == DevFile::DeviceType::CHAR){
        if(df->dev.CharDev) return df->dev.CharDev->Read(file, buffer, size);
        return 0;
    } else if(df->Type == DevFile::DeviceType::BLOCK){
        if(!df->dev.BlockDev) return 0;
        // Calculate sector range (assume 512-byte sectors)
        const U32 SECTOR = 512;
        U64 startByte = df->CurrentPosition;
        U64 endByte = startByte + (U64)size;
        U64 firstLBA = startByte / SECTOR;
        U32 offset = (U32)(startByte % SECTOR);
        U64 lastLBA = (endByte == 0) ? firstLBA : (endByte - 1) / SECTOR;
        U32 count = (U32)(lastLBA - firstLBA + 1);

        PageAlloc::DMAAlloc::DMABuffer *buf = nullptr;
        if(!df->dev.BlockDev->ReadSectors(firstLBA, count, &buf)) return 0;

        // Ensure we don't copy past the DMA buffer
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

        df->CurrentPosition += toCopy;
        return toCopy;
    }
    return 0;
}

U32 DevFS::Write(File *File, U8 *Buffer, U32 Size){
    if(!File || !Buffer || Size == 0) return 0;
    DevFile* df = static_cast<DevFile*>(File);

    if(df->Type == DevFile::DeviceType::CHAR){
        if(df->dev.CharDev) return df->dev.CharDev->Write(File, Buffer, Size);
        return 0;
    } else if(df->Type == DevFile::DeviceType::BLOCK){
        if(!df->dev.BlockDev) return 0;
        const U32 SECTOR = 512;
        U64 startByte = df->CurrentPosition;
        U64 endByte = startByte + (U64)Size;
        U64 firstLBA = startByte / SECTOR;
        U32 offset = (U32)(startByte % SECTOR);
        U64 lastLBA = (endByte == 0) ? firstLBA : (endByte - 1) / SECTOR;
        U32 count = (U32)(lastLBA - firstLBA + 1);

        // Allocate or obtain a buffer we can write back. For partial-sector
        // writes we must preserve existing bytes in the sectors we don't
        // intend to modify. Strategy:
        //  - If the write exactly covers whole sectors (offset==0 && Size >= count*SECTOR)
        //    then allocate a fresh DMA buffer and copy user's data directly.
        //  - Otherwise, read the existing sectors into a DMA buffer, modify the
        //    relevant bytes, and write the buffer back.

        SIZE_T bytesNeeded = (SIZE_T)count * SECTOR;
        PageAlloc::DMAAlloc::DMABuffer *buf = nullptr;

        if (offset == 0 && (SIZE_T)Size >= bytesNeeded) {
            // Full-sector write: create a buffer and copy directly.
            buf = PageAlloc::DMAAlloc::AllocateDMABytes(bytesNeeded);
            if (!buf) return 0;
            U8 *dst = (U8*)(UPTR)buf->VirtAddr;
            // Copy up to bytesNeeded (Size may be larger, but we only write bytesNeeded)
            U32 toCopy = (Size > (U32)bytesNeeded) ? (U32)bytesNeeded : Size;
            String::Memcpy(dst, Buffer, toCopy);
            // If Size < bytesNeeded, zero the remainder to avoid leaking data
            if ((SIZE_T)toCopy < bytesNeeded) {
                String::Memset(dst + toCopy, 0, bytesNeeded - toCopy);
            }
        } else {
            // Partial-sector write: read existing sectors, then patch.
            if(!df->dev.BlockDev->ReadSectors(firstLBA, count, &buf)) return 0;
            U8 *dst = (U8*)(UPTR)buf->VirtAddr + offset;
            // Only copy the user's Size bytes (won't touch bytes outside [offset, offset+Size))
            String::Memcpy(dst, Buffer, Size);
        }

        BOOL ok = df->dev.BlockDev->WriteSectors(firstLBA, count, buf);
        PageAlloc::DMAAlloc::FreeDMABuffer(buf);
        if(!ok) return 0;

        df->CurrentPosition += Size;
        return Size;
    }
    return 0;
}

INTN DevFS::Ioctl(File* file, U32 command, U64 arg){
    if(!file) return -1;
    DevFile* df = static_cast<DevFile*>(file);

    if(df->Type == DevFile::DeviceType::CHAR){
        if(df->dev.CharDev) return df->dev.CharDev->Ioctl(file, command, arg);
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
    U32 written = Read(file, buffer, size); // Use Write semantics
    (void)written;
    return TRUE;
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