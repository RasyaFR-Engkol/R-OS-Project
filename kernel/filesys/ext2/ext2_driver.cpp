#include "ext2_driver.hpp"
#include <filesystem/filesystem.hpp>
#define PRINTK_MODULE_NAME "EXT2FS"
#include <logging.hpp>
#include <string.hpp>
#include <mm.hpp>

EXT2FileSystem::EXT2FileSystem(){
    m_Partition = nullptr;
    m_BlockSize = 0;
}

EXT2FileSystem::~EXT2FileSystem() {
    // Kosongkan untuk sekarang
}

BOOL EXT2FileSystem::Mount(Partition *Part){
    Printk::Write(Printk::Level::LOG_INFO, "EXT2: Mounting EXT2 filesystem...\n");
    m_Partition = Part;

    PageAlloc::DMAAlloc::DMABuffer *Buffer = nullptr;
    if(!m_Partition->ReadSectors(2, 1, &Buffer)){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: Failed to read superblock sector.\n");
        return FALSE;
    }

    String::Memcpy(&m_Superblock, (VOID*)Buffer->VirtAddr, sizeof(EXT2::SuperBlock));

    PageAlloc::DMAAlloc::FreeDMABuffer(Buffer);

    if(m_Superblock.s_magic != EXT2_SUPER_MAGIC){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: Invalid superblock magic: 0x%X\n", m_Superblock.s_magic);
        m_Partition = nullptr;
        return FALSE;
    }

    m_BlockSize = 1024 << m_Superblock.s_log_block_size;
    Printk::Write(Printk::Level::LOG_INFO, "EXT2: Mounted successfully. Block Size: %u bytes\n", m_BlockSize);      

    m_Partition->SetReadWrite();

    return TRUE;
}

File* EXT2FileSystem::Open(const char* path) {
    Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Open(%s) not implemented.\n", path);
    return nullptr;
}

File* EXT2FileSystem::Create(const char *Path) {
    Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Create() not implemented.\n");
    return nullptr;
}

void EXT2FileSystem::Close(File* file) {
    Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Close() not implemented.\n");
    if(file) {
        delete file; // Asumsi 'File' di-alloc pakai 'new'
    }
}

U32 EXT2FileSystem::Read(File* file, U8* buffer, U32 size) {
    Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Read() not implemented.\n");
    return 0;
}

U32 EXT2FileSystem::Write(File *File, U8 *Buffer, U32 Size) {
    Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Write() not implemented.\n");
    return 0;
}

BOOL EXT2FileSystem::Delete(const char* path) {
    Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Delete() not implemented.\n");
    return FALSE;
}

BOOL EXT2FileSystem::Rename(const char* oldPath, const char* newPath) {
    Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Rename() not implemented.\n");
    return FALSE;
}

BOOL EXT2FileSystem::Seek(File* file, U64 position) {
    Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Seek() not implemented.\n");
    return FALSE;
}