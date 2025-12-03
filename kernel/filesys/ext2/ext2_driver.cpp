#define PRINTK_MODULE_NAME "EXT2FS"
#include "ext2_driver.hpp"
#include "ext2.hpp"
#include <rosval.h>
#include <filesystem/filesystem.hpp>
#include <logging.hpp>
#include <string.hpp>
#include <mm.hpp>
#include <rossys.hpp>
#include <rostime.hpp>

// Forward declarations for helpers defined later in this file
static VOID ParsePath(const CHAR8 *FullPath, CHAR8 *OutParentPath, CHAR8 *OutFileName);
static BOOL ext2_dirent_name_compare(const char* name1, U8 len1, const char* name2);
static U16 GetNeedRecLen(U8 NameLen);

EXT2FileSystem::EXT2FileSystem(){
    m_Partition = nullptr;
    m_BlockSize = 0;
    // debug hooks
    m_DebugFailAfter = 0;
    m_DebugWriteCounter = 0;
    m_DebugDefaultUid = 0xFFFFFFFF;
    m_DebugDefaultGid = 0xFFFFFFFF;
}

EXT2FileSystem::~EXT2FileSystem() {
    // Kosongkan untuk sekarang
}

BOOL EXT2FileSystem::ReadBlock(U32 blockNum, PageAlloc::DMAAlloc::DMABuffer** outBuffer) {
    if (blockNum == 0) {
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: ReadBlock(0) attempted. Block 0 is invalid.\n");
        return FALSE;
    }
    
    // Konversi block FS ke LBA
    // LBA = blockNum * (m_BlockSize / 512)
    U64 LBA = (U64)blockNum * m_SectorsPerBlock;
    
    // Panggil fungsi partisi
    return m_Partition->ReadSectors(LBA, m_SectorsPerBlock, outBuffer);
}

BOOL EXT2FileSystem::WriteBlock(U32 blockNum, PageAlloc::DMAAlloc::DMABuffer* buffer) {
    if (blockNum == 0) {
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: WriteBlock(0) attempted. Block 0 is invalid.\n");
        return FALSE;
    }

    U64 LBA = (U64)blockNum * m_SectorsPerBlock;

    // Debug failure injection: simulate WriteBlock failure after a configured number of writes
    if(m_DebugFailAfter != 0){
        m_DebugWriteCounter++;
        if(m_DebugWriteCounter >= m_DebugFailAfter){
            Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Debug - simulating WriteBlock failure at block %u (write #%u)\n", blockNum, m_DebugWriteCounter);
            return FALSE; // simulate write failure
        }
    }

    return m_Partition->WriteSectors(LBA, m_SectorsPerBlock, buffer);
}

BOOL EXT2FileSystem::Mount(Partition *Part){
    Printk::Write(Printk::Level::LOG_DEBUG, "EXT2: Mounting EXT2 filesystem...\n");
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
    m_SectorsPerBlock = m_BlockSize / 512;
    Printk::Write(Printk::Level::LOG_DEBUG, "EXT2: Mounted successfully. Block Size: %u bytes\n", m_BlockSize);      

    m_NumBlockGroups = (m_Superblock.s_blocks_count + m_Superblock.s_blocks_per_group - 1) 
                        / m_Superblock.s_blocks_per_group;

    Printk::Write(Printk::Level::LOG_DEBUG, "EXT2: Number of Block Groups: %u\n", m_NumBlockGroups);

    U32 BGDTStartBlocks = (m_BlockSize == 1024) ? 2 : 1;
    U32 BGDTSizeBytes = m_NumBlockGroups * sizeof(EXT2::BlockGroupDescriptor);
    m_BGDT_SizeInBlocks = (BGDTSizeBytes + m_BlockSize - 1) / m_BlockSize;

    Printk::Write(Printk::Level::LOG_DEBUG, "EXT2: BLOCK Size: %u bytes (%u blocks)\n", 
        m_BlockSize, m_BGDT_SizeInBlocks);
    m_BGDT = (EXT2::BlockGroupDescriptor*)Kmalloc::Alloc(m_BGDT_SizeInBlocks * m_BlockSize);
    if(!m_BGDT){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: Failed to allocate memory for BGDT.\n");
        m_Partition = nullptr;
        return FALSE;
    }

    Printk::Write(Printk::Level::LOG_DEBUG, "EXT2: Reading BGDT starting at block %u, size %u blocks\n", 
        BGDTStartBlocks, m_BGDT_SizeInBlocks);

    U8 *DMABufferAddr = (U8*)m_BGDT;
    for(U32 i = 0; i < m_BGDT_SizeInBlocks; i++){
        PageAlloc::DMAAlloc::DMABuffer *TempBuffer = nullptr;
        if(!ReadBlock(BGDTStartBlocks + i, &TempBuffer)) {
            Printk::Write(Printk::Level::LOG_ERR, "EXT2: Failed to read BGDT block %u\n", BGDTStartBlocks + i);
            Kmalloc::Free(m_BGDT);
            m_BGDT = nullptr;
            m_Partition = nullptr;
            return FALSE;
        }

        String::Memcpy(DMABufferAddr + (i * m_BlockSize), (U8*)TempBuffer->VirtAddr, m_BlockSize);  
        PageAlloc::DMAAlloc::FreeDMABuffer(TempBuffer);
    }

    Printk::Write(Printk::Level::LOG_DEBUG, "EXT2: BGDT read successfully.\n");
    m_Partition->SetReadWrite();

    return TRUE;
}

// Truncate: shrink (and allow grow as simple size update) file to 'size'
BOOL EXT2FileSystem::Truncate(File* file, U64 size){
    if(!file) return FALSE;
    U32 inodeNum = file->Internal_StartCluster;
    if(inodeNum == 0) return FALSE;

    EXT2::Inode inode;
    if(!ReadInode(inodeNum, &inode)) return FALSE;

    // If growing or equal: update size only (blocks allocated on demand)
    if(size >= inode.i_size){
        inode.i_size = (U32)size;
        if(!WriteInode(inodeNum, &inode)) return FALSE;
        PersistSuperblockAndBGDT();
        file->FileSize = inode.i_size;
        return TRUE;
    }

    // Shrink: free blocks beyond new size
    U32 oldBlocks = (inode.i_size + m_BlockSize - 1) / m_BlockSize;
    U32 newBlocks = (size + m_BlockSize - 1) / m_BlockSize;
    if(newBlocks >= oldBlocks){
        inode.i_size = (U32)size;
        if(!WriteInode(inodeNum, &inode)) return FALSE;
        PersistSuperblockAndBGDT();
        file->FileSize = inode.i_size;
        return TRUE;
    }

    U32 EntriesPerBlock = m_BlockSize / sizeof(U32);

    // Free direct
    for(U32 i = newBlocks; i < 12 && i < oldBlocks; i++){
        if(inode.i_block[i] != 0){
            FreeBlock(inode.i_block[i]);
            inode.i_block[i] = 0;
            inode.i_blocks -= (m_BlockSize / 512);
        }
    }

    U32 singleStart = 12;
    U32 singleCount = EntriesPerBlock;
    U32 doubleStart = singleStart + singleCount;
    U32 doubleCount = EntriesPerBlock * EntriesPerBlock;
    U32 tripleStart = doubleStart + doubleCount;

    // Single indirect
    if(inode.i_block[12] != 0){
        PageAlloc::DMAAlloc::DMABuffer* ibuf = nullptr;
        if(ReadBlock(inode.i_block[12], &ibuf)){
            U32* ptrs = (U32*)ibuf->VirtAddr;
            for(U32 j = 0; j < EntriesPerBlock; j++){
                U32 globalIdx = singleStart + j;
                if(globalIdx >= newBlocks && ptrs[j] != 0){
                    FreeBlock(ptrs[j]);
                    ptrs[j] = 0;
                    inode.i_blocks -= (m_BlockSize / 512);
                }
            }
            // check if indirect block empty
            BOOL any = FALSE;
            for(U32 j=0;j<EntriesPerBlock;j++) if(((U32*)ibuf->VirtAddr)[j] != 0) { any = TRUE; break; }
            if(any){ WriteBlock(inode.i_block[12], ibuf); }
            else { PageAlloc::DMAAlloc::FreeDMABuffer(ibuf); FreeBlock(inode.i_block[12]); inode.i_block[12] = 0; ibuf = nullptr; }
            if(ibuf) PageAlloc::DMAAlloc::FreeDMABuffer(ibuf);
        }
    }

    // Double indirect
    if(inode.i_block[13] != 0){
        PageAlloc::DMAAlloc::DMABuffer* dbuf = nullptr;
        if(ReadBlock(inode.i_block[13], &dbuf)){
            U32* lvl1 = (U32*)dbuf->VirtAddr;
            for(U32 i1 = 0; i1 < EntriesPerBlock; i1++){
                if(lvl1[i1] == 0) continue;
                PageAlloc::DMAAlloc::DMABuffer* sbuf = nullptr;
                if(!ReadBlock(lvl1[i1], &sbuf)) continue;
                U32* ptrs = (U32*)sbuf->VirtAddr;
                for(U32 j = 0; j < EntriesPerBlock; j++){
                    U32 globalIdx = doubleStart + (i1 * EntriesPerBlock) + j;
                    if(globalIdx >= newBlocks && ptrs[j] != 0){
                        FreeBlock(ptrs[j]);
                        ptrs[j] = 0;
                        inode.i_blocks -= (m_BlockSize / 512);
                    }
                }
                // check single-indirect empty
                BOOL any2 = FALSE;
                for(U32 j=0;j<EntriesPerBlock;j++) if(((U32*)sbuf->VirtAddr)[j] != 0) { any2 = TRUE; break; }
                if(any2){ WriteBlock(lvl1[i1], sbuf); }
                else { PageAlloc::DMAAlloc::FreeDMABuffer(sbuf); FreeBlock(lvl1[i1]); lvl1[i1] = 0; sbuf = nullptr; }
                if(sbuf) PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);
            }
            BOOL any1 = FALSE;
            for(U32 i1=0;i1<EntriesPerBlock;i1++) if(((U32*)dbuf->VirtAddr)[i1] != 0) { any1 = TRUE; break; }
            if(any1){ WriteBlock(inode.i_block[13], dbuf); }
            else { PageAlloc::DMAAlloc::FreeDMABuffer(dbuf); FreeBlock(inode.i_block[13]); inode.i_block[13] = 0; dbuf = nullptr; }
            if(dbuf) PageAlloc::DMAAlloc::FreeDMABuffer(dbuf);
        }
    }

    // Triple indirect
    if(inode.i_block[14] != 0){
        PageAlloc::DMAAlloc::DMABuffer* tbuf = nullptr;
        if(ReadBlock(inode.i_block[14], &tbuf)){
            U32* lvl1 = (U32*)tbuf->VirtAddr;
            for(U32 i1 = 0; i1 < EntriesPerBlock; i1++){
                if(lvl1[i1] == 0) continue;
                PageAlloc::DMAAlloc::DMABuffer* mbuf = nullptr;
                if(!ReadBlock(lvl1[i1], &mbuf)) continue;
                U32* lvl2 = (U32*)mbuf->VirtAddr;
                for(U32 i2 = 0; i2 < EntriesPerBlock; i2++){
                    if(lvl2[i2] == 0) continue;
                    PageAlloc::DMAAlloc::DMABuffer* sbuf = nullptr;
                    if(!ReadBlock(lvl2[i2], &sbuf)) continue;
                    U32* ptrs = (U32*)sbuf->VirtAddr;
                    for(U32 j = 0; j < EntriesPerBlock; j++){
                        U64 idx = (U64)(tripleStart) + (U64)i1 * (U64)doubleCount + (U64)i2 * (U64)EntriesPerBlock + (U64)j;
                        if(idx >= (U64)newBlocks && ptrs[j] != 0){
                            FreeBlock(ptrs[j]);
                            ptrs[j] = 0;
                            inode.i_blocks -= (m_BlockSize / 512);
                        }
                    }
                    BOOL any3 = FALSE;
                    for(U32 j=0;j<EntriesPerBlock;j++) if(((U32*)sbuf->VirtAddr)[j] != 0) { any3 = TRUE; break; }
                    if(any3){ WriteBlock(lvl2[i2], sbuf); }
                    else { PageAlloc::DMAAlloc::FreeDMABuffer(sbuf); FreeBlock(lvl2[i2]); lvl2[i2] = 0; sbuf = nullptr; }
                    if(sbuf) PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);
                }
                BOOL any2 = FALSE;
                for(U32 i2=0;i2<EntriesPerBlock;i2++) if(((U32*)mbuf->VirtAddr)[i2] != 0) { any2 = TRUE; break; }
                if(any2){ WriteBlock(lvl1[i1], mbuf); }
                else { PageAlloc::DMAAlloc::FreeDMABuffer(mbuf); FreeBlock(lvl1[i1]); lvl1[i1] = 0; mbuf = nullptr; }
                if(mbuf) PageAlloc::DMAAlloc::FreeDMABuffer(mbuf);
            }
            BOOL any1 = FALSE;
            for(U32 i1=0;i1<EntriesPerBlock;i1++) if(((U32*)tbuf->VirtAddr)[i1] != 0) { any1 = TRUE; break; }
            if(any1){ WriteBlock(inode.i_block[14], tbuf); }
            else { PageAlloc::DMAAlloc::FreeDMABuffer(tbuf); FreeBlock(inode.i_block[14]); inode.i_block[14] = 0; tbuf = nullptr; }
            if(tbuf) PageAlloc::DMAAlloc::FreeDMABuffer(tbuf);
        }
    }

    inode.i_size = (U32)size;
    if(!WriteInode(inodeNum, &inode)) return FALSE;
    PersistSuperblockAndBGDT();
    file->FileSize = inode.i_size;
    return TRUE;
}

// Make directory
BOOL EXT2FileSystem::MKDir(const char* path){
    if(!path || String::Strlen(path) == 0) return FALSE;

    CHAR8 ParentPath[256]; CHAR8 DirName[256];
    ParsePath((const CHAR8*)path, ParentPath, DirName);

    EXT2::Inode parentInode;
    U32 parentInodeNum = FindInodeForPath((const char*)ParentPath, &parentInode);
    if(parentInodeNum == 0) return FALSE;
    if(!(parentInode.i_mode & EXT2_S_IFDIR)) return FALSE;

    EXT2::DirectoryEntry existing;
    if(FindEntryInDirectory(&parentInode, (const CHAR8*)DirName, &existing)) return FALSE;

    U32 newInodeNum = AllocateInode();
    if(newInodeNum == 0) return FALSE;
    U32 newBlock = AllocateBlock();
    if(newBlock == 0){ FreeInode(newInodeNum); return FALSE; }

    EXT2::Inode newInode;
    String::Memset(&newInode, 0, sizeof(newInode));
    newInode.i_mode = EXT2_S_IFDIR | 0755;
    // Use debug-configured default owner for directories if present
    newInode.i_uid = (m_DebugDefaultUid != 0xFFFFFFFF) ? m_DebugDefaultUid : 0;
    newInode.i_gid = (m_DebugDefaultGid != 0xFFFFFFFF) ? m_DebugDefaultGid : 0;
    newInode.i_size = m_BlockSize;
    newInode.i_links_count = 2;
    newInode.i_blocks = (m_BlockSize / 512);
    newInode.i_block[0] = newBlock;

    PageAlloc::DMAAlloc::DMABuffer* dbuf = PageAlloc::DMAAlloc::AllocateDMABytes(m_BlockSize);
    if(!dbuf){ FreeBlock(newBlock); FreeInode(newInodeNum); return FALSE; }
    String::Memset((U8*)dbuf->VirtAddr, 0, m_BlockSize);

    U16 dot_len = GetNeedRecLen(1);
    struct DirHdr { U32 inode; U16 rec_len; U8 name_len; U8 file_type; } d1;
    d1.inode = newInodeNum; d1.rec_len = dot_len; d1.name_len = 1; d1.file_type = 2;
    String::Memcpy((U8*)dbuf->VirtAddr, &d1, sizeof(d1));
    String::Memcpy((U8*)dbuf->VirtAddr + sizeof(d1), ".", 1);

    U16 dotdot_len = (U16)(m_BlockSize - dot_len);
    struct DirHdr d2;
    d2.inode = parentInodeNum; d2.rec_len = dotdot_len; d2.name_len = 2; d2.file_type = 2;
    String::Memcpy((U8*)dbuf->VirtAddr + dot_len, &d2, sizeof(d2));
    String::Memcpy((U8*)dbuf->VirtAddr + dot_len + sizeof(d2), "..", 2);

    if(!WriteBlock(newBlock, dbuf)){ PageAlloc::DMAAlloc::FreeDMABuffer(dbuf); FreeBlock(newBlock); FreeInode(newInodeNum); return FALSE; }
    PageAlloc::DMAAlloc::FreeDMABuffer(dbuf);

    if(!WriteInode(newInodeNum, &newInode)){ FreeBlock(newBlock); FreeInode(newInodeNum); return FALSE; }

    if(!AddEntryToDirectory(&parentInode, newInodeNum, (const CHAR8*)DirName, EXT2_S_IFDIR, parentInodeNum)){
        FreeBlock(newBlock); FreeInode(newInodeNum); return FALSE;
    }

    parentInode.i_links_count++;
    WriteInode(parentInodeNum, &parentInode);

    U32 group = (newInodeNum - 1) / m_Superblock.s_inodes_per_group;
    if(group < m_NumBlockGroups) m_BGDT[group].bg_used_dirs_count++;

    PersistSuperblockAndBGDT();
    return TRUE;
}

// Remove directory
BOOL EXT2FileSystem::RMDir(const char* path){
    if(!path || String::Strlen(path) == 0) return FALSE;
    CHAR8 ParentPath[256]; CHAR8 Name[256];
    ParsePath((const CHAR8*)path, ParentPath, Name);

    EXT2::Inode parentInode; U32 parentInodeNum = FindInodeForPath((const char*)ParentPath, &parentInode);
    if(parentInodeNum == 0) return FALSE;

    EXT2::DirectoryEntry ent;
    if(!FindEntryInDirectory(&parentInode, (const CHAR8*)Name, &ent)) return FALSE;

    U32 targetInodeNum = ent.inode;
    EXT2::Inode targetInode;
    if(!ReadInode(targetInodeNum, &targetInode)) return FALSE;
    if(!(targetInode.i_mode & EXT2_S_IFDIR)) return FALSE;

    // ensure empty (only . and ..) - check direct and indirect directory data blocks
    auto block_has_non_dot = [&](U32 blockNum)->BOOL{
        if(blockNum == 0) return FALSE;
        PageAlloc::DMAAlloc::DMABuffer* b = nullptr;
        if(!ReadBlock(blockNum, &b)) return FALSE; // treat unreadable as absent
        U8* data = (U8*)b->VirtAddr;
        U32 off = 0;
        struct DirHdr { U32 inode; U16 rec_len; U8 name_len; U8 file_type; } hdr;
        while(off < m_BlockSize){
            String::Memcpy(&hdr, data + off, sizeof(hdr));
            if(hdr.rec_len == 0) break;
            if(hdr.inode != 0){
                U8 namebuf[256]; U32 copyLen = hdr.name_len; if(copyLen > 255) copyLen = 255;
                String::Memcpy(namebuf, data + off + sizeof(hdr), copyLen); namebuf[copyLen] = '\0';
                if(!(copyLen == 1 && namebuf[0]=='.') && !(copyLen==2 && namebuf[0]=='.' && namebuf[1]=='.')){
                    PageAlloc::DMAAlloc::FreeDMABuffer(b);
                    return TRUE;
                }
            }
            off += hdr.rec_len;
        }
        PageAlloc::DMAAlloc::FreeDMABuffer(b);
        return FALSE;
    };

    // 1) direct blocks
    for(int i=0;i<12;i++){
        if(targetInode.i_block[i] == 0) continue;
        if(block_has_non_dot(targetInode.i_block[i])) return FALSE;
    }

    U32 EPerBlock = m_BlockSize / sizeof(U32);

    // 2) single indirect
    if(targetInode.i_block[12] != 0){
        PageAlloc::DMAAlloc::DMABuffer* ibuf = nullptr;
        if(ReadBlock(targetInode.i_block[12], &ibuf)){
            U32* ptrs = (U32*)ibuf->VirtAddr;
            for(U32 j=0;j<EPerBlock;j++){
                if(ptrs[j] == 0) continue;
                if(block_has_non_dot(ptrs[j])){ PageAlloc::DMAAlloc::FreeDMABuffer(ibuf); return FALSE; }
            }
            PageAlloc::DMAAlloc::FreeDMABuffer(ibuf);
        }
    }

    // 3) double indirect
    if(targetInode.i_block[13] != 0){
        PageAlloc::DMAAlloc::DMABuffer* dbuf2 = nullptr;
        if(ReadBlock(targetInode.i_block[13], &dbuf2)){
            U32* lvl1 = (U32*)dbuf2->VirtAddr;
            for(U32 i1=0;i1<EPerBlock;i1++){
                if(lvl1[i1] == 0) continue;
                PageAlloc::DMAAlloc::DMABuffer* sbuf = nullptr;
                if(ReadBlock(lvl1[i1], &sbuf)){
                    U32* ptrs = (U32*)sbuf->VirtAddr;
                    for(U32 j=0;j<EPerBlock;j++){
                        if(ptrs[j] == 0) continue;
                        if(block_has_non_dot(ptrs[j])){ PageAlloc::DMAAlloc::FreeDMABuffer(sbuf); PageAlloc::DMAAlloc::FreeDMABuffer(dbuf2); return FALSE; }
                    }
                    PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);
                }
            }
            PageAlloc::DMAAlloc::FreeDMABuffer(dbuf2);
        }
    }

    // 4) triple indirect
    if(targetInode.i_block[14] != 0){
        PageAlloc::DMAAlloc::DMABuffer* tbuf = nullptr;
        if(ReadBlock(targetInode.i_block[14], &tbuf)){
            U32* lvl1 = (U32*)tbuf->VirtAddr;
            for(U32 i1=0;i1<EPerBlock;i1++){
                if(lvl1[i1] == 0) continue;
                PageAlloc::DMAAlloc::DMABuffer* mbuf = nullptr;
                if(ReadBlock(lvl1[i1], &mbuf)){
                    U32* lvl2 = (U32*)mbuf->VirtAddr;
                    for(U32 i2=0;i2<EPerBlock;i2++){
                        if(lvl2[i2] == 0) continue;
                        PageAlloc::DMAAlloc::DMABuffer* sbuf = nullptr;
                        if(ReadBlock(lvl2[i2], &sbuf)){
                            U32* ptrs = (U32*)sbuf->VirtAddr;
                            for(U32 j=0;j<EPerBlock;j++){
                                if(ptrs[j] == 0) continue;
                                if(block_has_non_dot(ptrs[j])){ PageAlloc::DMAAlloc::FreeDMABuffer(sbuf); PageAlloc::DMAAlloc::FreeDMABuffer(mbuf); PageAlloc::DMAAlloc::FreeDMABuffer(tbuf); return FALSE; }
                            }
                            PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);
                        }
                    }
                    PageAlloc::DMAAlloc::FreeDMABuffer(mbuf);
                }
            }
            PageAlloc::DMAAlloc::FreeDMABuffer(tbuf);
        }
    }

    if(!RemoveEntryFromDirectory(&parentInode, (const CHAR8*)Name)) return FALSE;

    // free blocks and indirects
    if(targetInode.i_block[0] != 0){
        for(int i=0;i<12;i++){ if(targetInode.i_block[i] != 0){ FreeBlock(targetInode.i_block[i]); targetInode.i_block[i]=0; } }
        U32 EntriesPerBlock = m_BlockSize / sizeof(U32);
        if(targetInode.i_block[12] != 0){ PageAlloc::DMAAlloc::DMABuffer* ibuf=nullptr; if(ReadBlock(targetInode.i_block[12], &ibuf)){ U32* ptrs=(U32*)ibuf->VirtAddr; for(U32 j=0;j<EntriesPerBlock;j++) if(ptrs[j]!=0){ FreeBlock(ptrs[j]); ptrs[j]=0; } PageAlloc::DMAAlloc::FreeDMABuffer(ibuf);} FreeBlock(targetInode.i_block[12]); targetInode.i_block[12]=0; }
        if(targetInode.i_block[13] != 0){ PageAlloc::DMAAlloc::DMABuffer* dbuf2=nullptr; if(ReadBlock(targetInode.i_block[13], &dbuf2)){ U32* lvl1=(U32*)dbuf2->VirtAddr; for(U32 i1=0;i1<EntriesPerBlock;i1++){ if(lvl1[i1]==0) continue; PageAlloc::DMAAlloc::DMABuffer* sbuf=nullptr; if(ReadBlock(lvl1[i1], &sbuf)){ U32* ptrs=(U32*)sbuf->VirtAddr; for(U32 j=0;j<EntriesPerBlock;j++) if(ptrs[j]!=0){ FreeBlock(ptrs[j]); ptrs[j]=0; } PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);} FreeBlock(lvl1[i1]); } PageAlloc::DMAAlloc::FreeDMABuffer(dbuf2);} FreeBlock(targetInode.i_block[13]); targetInode.i_block[13]=0; }
        if(targetInode.i_block[14] != 0){ PageAlloc::DMAAlloc::DMABuffer* tbuf=nullptr; if(ReadBlock(targetInode.i_block[14], &tbuf)){ U32* lvl1=(U32*)tbuf->VirtAddr; for(U32 i1=0;i1<EntriesPerBlock;i1++){ if(lvl1[i1]==0) continue; PageAlloc::DMAAlloc::DMABuffer* mbuf=nullptr; if(ReadBlock(lvl1[i1], &mbuf)){ U32* lvl2=(U32*)mbuf->VirtAddr; for(U32 i2=0;i2<EntriesPerBlock;i2++){ if(lvl2[i2]==0) continue; PageAlloc::DMAAlloc::DMABuffer* sbuf=nullptr; if(ReadBlock(lvl2[i2], &sbuf)){ U32* ptrs=(U32*)sbuf->VirtAddr; for(U32 j=0;j<EntriesPerBlock;j++) if(ptrs[j]!=0){ FreeBlock(ptrs[j]); ptrs[j]=0; } PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);} FreeBlock(lvl2[i2]); } PageAlloc::DMAAlloc::FreeDMABuffer(mbuf);} FreeBlock(lvl1[i1]); } PageAlloc::DMAAlloc::FreeDMABuffer(tbuf);} FreeBlock(targetInode.i_block[14]); targetInode.i_block[14]=0; }
    }

    FreeInode(targetInodeNum);
    if(parentInode.i_links_count > 0) parentInode.i_links_count--;
    WriteInode(parentInodeNum, &parentInode);

    U32 group = (targetInodeNum - 1) / m_Superblock.s_inodes_per_group;
    if(group < m_NumBlockGroups && m_BGDT[group].bg_used_dirs_count > 0) m_BGDT[group].bg_used_dirs_count--;

    PersistSuperblockAndBGDT();
    return TRUE;
}

// Flush metadata for file
BOOL EXT2FileSystem::Flush(File* file){
    if(!file) return FALSE;
    U32 inodeNum = file->Internal_StartCluster;
    if(inodeNum == 0) return FALSE;
    EXT2::Inode inode;
    if(!ReadInode(inodeNum, &inode)) return FALSE;
    if(!WriteInode(inodeNum, &inode)) return FALSE;
    PersistSuperblockAndBGDT();
    return TRUE;
}

// Append: seek to EOF and write data. Return TRUE if all bytes written.
BOOL EXT2FileSystem::Append(File* file, U8* buffer, U32 size){
    if(!file || !buffer || size == 0) return FALSE;
    if(file->IsDirectory) return FALSE;

    // Seek to EOF
    if(!Seek(file, file->FileSize)) return FALSE;

    U32 written = Write(file, buffer, size);
    return (written == size) ? TRUE : FALSE;
}

INTN EXT2FileSystem::ReadDir(File* dirFile, void* buffer, U32 bufferSize){
    if(!dirFile || !buffer) return -1;
    if(!dirFile->IsDirectory) return -1;
    if(bufferSize == 0) return 0;

    U32 inodeNum = dirFile->Internal_StartCluster;
    if(inodeNum == 0) return -1;

    EXT2::Inode dirInode;
    if(!ReadInode(inodeNum, &dirInode)) return -1;

    char* out = (char*)buffer;
    U32 remain = bufferSize;

    PageAlloc::DMAAlloc::DMABuffer *DirDataBuffer = nullptr;
    bool stop = false;

    // Use CurrentPosition as number of entries already returned
    U64 skip = dirFile->CurrentPosition;
    U64 seen = 0;

    // helper to scan a single directory data block
    auto scan_dir_block = [&](U32 blockNum)->BOOL{
        if(blockNum == 0) return TRUE; // treat as empty block (continue)
        if(!ReadBlock(blockNum, &DirDataBuffer)) return FALSE;
        U8 *DirPtr = (U8*)DirDataBuffer->VirtAddr;
        U32 Offset = 0;
        struct DirentHeader { U32 inode; U16 rec_len; U8 name_len; U8 file_type; } hdr;
        while(Offset < m_BlockSize){
            U8* entryPtr = DirPtr + Offset;
            String::Memcpy(&hdr, entryPtr, sizeof(hdr));
            if(hdr.rec_len == 0) break;
            if(hdr.inode != 0 && hdr.name_len > 0){
                U32 namelen = hdr.name_len;
                if(namelen > 255) namelen = 255;
                // increment seen count for this entry
                U64 idx = seen++;
                if(idx < skip){ Offset += hdr.rec_len; continue; }
                // need space for name + NUL
                U32 need = namelen + 1;
                if(need > remain){ PageAlloc::DMAAlloc::FreeDMABuffer(DirDataBuffer); return FALSE; }
                String::Memcpy((U8*)out, entryPtr + sizeof(hdr), namelen);
                out += namelen;
                *out++ = '\0';
                remain -= need;
            }
            Offset += hdr.rec_len;
        }
        PageAlloc::DMAAlloc::FreeDMABuffer(DirDataBuffer);
        return TRUE;
    };

    // 1) direct blocks
    for(int i = 0; i < 12 && !stop; i++){
        if(dirInode.i_block[i] == 0) continue;
        if(!scan_dir_block(dirInode.i_block[i])) stop = true;
    }

    U32 EntriesPerBlock = m_BlockSize / sizeof(U32);

    // single indirect
    if(!stop && dirInode.i_block[12] != 0){
        PageAlloc::DMAAlloc::DMABuffer* ibuf = nullptr;
        if(ReadBlock(dirInode.i_block[12], &ibuf)){
            U32* ptrs = (U32*)ibuf->VirtAddr;
            for(U32 j = 0; j < EntriesPerBlock && !stop; j++){
                if(ptrs[j] == 0) continue;
                if(!scan_dir_block(ptrs[j])){ stop = true; break; }
            }
            PageAlloc::DMAAlloc::FreeDMABuffer(ibuf);
        }
    }

    // double indirect
    if(!stop && dirInode.i_block[13] != 0){
        PageAlloc::DMAAlloc::DMABuffer* dbuf = nullptr;
        if(ReadBlock(dirInode.i_block[13], &dbuf)){
            U32* lvl1 = (U32*)dbuf->VirtAddr;
            for(U32 i1 = 0; i1 < EntriesPerBlock && !stop; i1++){
                if(lvl1[i1] == 0) continue;
                PageAlloc::DMAAlloc::DMABuffer* sbuf = nullptr;
                if(ReadBlock(lvl1[i1], &sbuf)){
                    U32* ptrs = (U32*)sbuf->VirtAddr;
                    for(U32 j = 0; j < EntriesPerBlock; j++){
                        if(ptrs[j] == 0) continue;
                        if(!scan_dir_block(ptrs[j])){ PageAlloc::DMAAlloc::FreeDMABuffer(sbuf); PageAlloc::DMAAlloc::FreeDMABuffer(dbuf); stop = true; break; }
                    }
                    if(!stop) PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);
                }
            }
            if(!stop) PageAlloc::DMAAlloc::FreeDMABuffer(dbuf);
        }
    }

    // triple indirect
    if(!stop && dirInode.i_block[14] != 0){
        PageAlloc::DMAAlloc::DMABuffer* tbuf = nullptr;
        if(ReadBlock(dirInode.i_block[14], &tbuf)){
            U32* lvl1 = (U32*)tbuf->VirtAddr;
            for(U32 i1 = 0; i1 < EntriesPerBlock && !stop; i1++){
                if(lvl1[i1] == 0) continue;
                PageAlloc::DMAAlloc::DMABuffer* mbuf = nullptr;
                if(ReadBlock(lvl1[i1], &mbuf)){
                    U32* lvl2 = (U32*)mbuf->VirtAddr;
                    for(U32 i2 = 0; i2 < EntriesPerBlock && !stop; i2++){
                        if(lvl2[i2] == 0) continue;
                        PageAlloc::DMAAlloc::DMABuffer* sbuf = nullptr;
                        if(ReadBlock(lvl2[i2], &sbuf)){
                            U32* ptrs = (U32*)sbuf->VirtAddr;
                            for(U32 j = 0; j < EntriesPerBlock; j++){
                                if(ptrs[j] == 0) continue;
                                if(!scan_dir_block(ptrs[j])){ PageAlloc::DMAAlloc::FreeDMABuffer(sbuf); PageAlloc::DMAAlloc::FreeDMABuffer(mbuf); PageAlloc::DMAAlloc::FreeDMABuffer(tbuf); stop = true; break; }
                            }
                            if(!stop) PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);
                        }
                    }
                    if(!stop) PageAlloc::DMAAlloc::FreeDMABuffer(mbuf);
                }
            }
            if(!stop) PageAlloc::DMAAlloc::FreeDMABuffer(tbuf);
        }
    }

    // update file position to number of entries seen
    dirFile->CurrentPosition = seen;

    U32 used = bufferSize - remain;
    return (INTN)used;
}

File* EXT2FileSystem::Open(const char* path) {
    EXT2::Inode inode;
    U32 InodeNum = FindInodeForPath(path, &inode);

    if(InodeNum == 0){
        Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Open - File not found: %s\n", path);
        return nullptr;
    }

    File *file = new File();
    if(!file){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: Open - Failed to allocate File object for %s\n", path);
        return nullptr;
    }

    String::Strcpy(file->FileName, path); // TODO: Seharusnya nama file, bukan nama path lengkap
    file->FileSize = inode.i_size;
    file->IsDirectory = (inode.i_mode & EXT2_S_IFDIR) ? TRUE : FALSE;
    file->CurrentPosition = 0;
    file->FSOwner = this;

    file->Internal_StartCluster = InodeNum;
    file->RefCount = 1; // Init RefCount

    Printk::Write(Printk::Level::LOG_DEBUG, "EXT2: Opened file %s (Inode %u, Size %u bytes)\n",
        path, InodeNum, inode.i_size);

    return file;
}

File* EXT2FileSystem::Create(const char *Path) {
    Printk::Write(Printk::Level::LOG_NOTICE, "EXT2: Create %s \n", Path);

    if(!Path || String::Strlen(Path) == 0) return nullptr;

    // Parse path into parent path and filename
    CHAR8 ParentPath[256];
    CHAR8 FileName[256];
    ParsePath((const CHAR8*)Path, ParentPath, FileName);

    // Find parent inode
    EXT2::Inode parentInode;
    U32 parentInodeNum = FindInodeForPath((const char*)ParentPath, &parentInode);
    if(parentInodeNum == 0){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: Create - parent path not found: %s\n", ParentPath);
        return nullptr;
    }

    if(!(parentInode.i_mode & EXT2_S_IFDIR)){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: Create - parent is not a directory: %s\n", ParentPath);
        return nullptr;
    }

    // Check if entry already exists
    EXT2::DirectoryEntry existing;
    if(FindEntryInDirectory(&parentInode, (const CHAR8*)FileName, &existing)){
        Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Create - file already exists: %s/%s\n", ParentPath, FileName);
        // Open existing file and return handle
        char full[512];
        String::Strcpy(full, ParentPath);
        if(full[String::Strlen(full)-1] != '/') String::Strcat(full, "/");
        String::Strcat(full, FileName);
        return Open(full);
    }

    // Allocate inode
    U32 newInodeNum = AllocateInode();
    if(newInodeNum == 0){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: Create - failed to allocate inode for %s\n", Path);
        return nullptr;
    }

    // Prepare new inode structure

    auto RTCTime = Arch::CMOS::ReadRTC();
    U32 CurrentTimestamp = Arch::Time::RTCToEpoch(RTCTime);

    EXT2::Inode newInode;
    String::Memset(&newInode, 0, sizeof(EXT2::Inode));
    newInode.i_mode = EXT2_S_IFREG | 0644; // regular file + permissions
    // Use debug-configured default owner if present, otherwise keep root (0)
    newInode.i_uid = (m_DebugDefaultUid != 0xFFFFFFFF) ? m_DebugDefaultUid : 0;
    newInode.i_gid = (m_DebugDefaultGid != 0xFFFFFFFF) ? m_DebugDefaultGid : 0;
    newInode.i_size = 0;
    newInode.i_links_count = 1;
    newInode.i_blocks = 0;
    newInode.i_generation = 0;
    newInode.i_flags = 0;
    newInode.i_atime = CurrentTimestamp;
    newInode.i_ctime = CurrentTimestamp;
    newInode.i_mtime = CurrentTimestamp;
    newInode.i_dtime = 0;

    // write inode to disk
    if(!WriteInode(newInodeNum, &newInode)){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: Create - failed to write inode %u for %s\n", newInodeNum, Path);
        return nullptr;
    }

    // Add directory entry to parent directory
    if(!AddEntryToDirectory(&parentInode, newInodeNum, (const CHAR8*)FileName, EXT2_S_IFREG, parentInodeNum)){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: Create - failed to add dir entry %s to %s\n", FileName, ParentPath);
        return nullptr;
    }

    // Persist parent inode changes
    if(!WriteInode(parentInodeNum, &parentInode)){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: Create - failed to write parent inode %u for %s\n", parentInodeNum, ParentPath);
        // Not fatal for now, but warn
    }

    // Persist BGDT to disk (BGDT location depends on block size)
    U32 BGDTStartBlocks = (m_BlockSize == 1024) ? 2 : 1;
    for(U32 i = 0; i < m_BGDT_SizeInBlocks; i++){
        PageAlloc::DMAAlloc::DMABuffer *tmp = PageAlloc::DMAAlloc::AllocateDMABytes(m_BlockSize);
        if(!tmp){
            Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Create - failed allocate DMABuffer to write BGDT block %u\n", i);
            break;
        }
        U8 *src = ((U8*)m_BGDT) + (i * m_BlockSize);
        String::Memcpy((U8*)tmp->VirtAddr, src, m_BlockSize);
        if(!WriteBlock(BGDTStartBlocks + i, tmp)){
            Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Create - failed write BGDT block %u\n", BGDTStartBlocks + i);
        }
        PageAlloc::DMAAlloc::FreeDMABuffer(tmp);
    }

    // Return File* handle for the new file
    File *file = new File();
    if(!file){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: Create - failed to allocate File object for %s\n", Path);
        return nullptr;
    }

    // Fill minimal file info
    String::Strcpy(file->FileName, Path);
    file->FileSize = 0;
    file->IsDirectory = FALSE;
    file->CurrentPosition = 0;
    file->FSOwner = this;
    file->Internal_StartCluster = newInodeNum;
    file->RefCount = 1; // Init RefCount

    Printk::Write(Printk::Level::LOG_DEBUG, "EXT2: Created file %s (Inode %u)\n", Path, newInodeNum);

    return file;
}

// Copy a file within the filesystem: srcPath and destPath are relative paths starting with '/'
BOOL EXT2FileSystem::Cp(const char* srcPath, const char* destPath){
    if(!srcPath || !destPath) return FALSE;
    // Open source
    File* src = Open(srcPath);
    if(!src) return FALSE;
    if(src->IsDirectory){ Close(src); return FALSE; }

    // Create destination (will fail if parent missing or entry exists)
    File* dst = Create(destPath);
    if(!dst){ Close(src); return FALSE; }

    // Buffer size: use block size or 4096
    U32 bufSize = (m_BlockSize && m_BlockSize <= 65536) ? m_BlockSize : 4096;
    U8* buf = (U8*)Kmalloc::Alloc(bufSize);
    if(!buf){ Close(src); Close(dst); return FALSE; }

    U32 r;
    while((r = Read(src, buf, bufSize)) > 0){
        U32 w = Write(dst, buf, r);
        if(w != r){ Kmalloc::Free(buf); Close(src); Close(dst); return FALSE; }
    }

    Kmalloc::Free(buf);
    Close(src);
    Close(dst);
    return TRUE;
}

void EXT2FileSystem::Close(File* file) {
    if(!file) return;

    // If this file belongs to this FS, attempt to flush metadata
    if(file->FSOwner == this){
        U32 inodeNum = file->Internal_StartCluster;
        if(inodeNum != 0){
            EXT2::Inode inode;
            if(ReadInode(inodeNum, &inode)){
                // update timestamps: modification and change time
                U64 now = Arch::Time::NowTicks();
                // convert ticks to u32 (best-effort)
                inode.i_mtime = (U32)now;
                inode.i_ctime = (U32)now;
                // update atime if file was read
                inode.i_atime = (U32)now;

                if(!WriteInode(inodeNum, &inode)){
                    Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Close - failed to persist inode %u\n", inodeNum);
                }
            }
        }
    }

    // Free file object
    delete file;
}

U32 EXT2FileSystem::Read(File* file, U8* buffer, U32 size) {
    if(!file || !buffer || size == 0){
        return 0;
    }

    if(file->IsDirectory){
        Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Read() called on a directory. Returning Directory Handle\n");
    }

    U32 InodeNum = file->Internal_StartCluster;
    EXT2::Inode inode;
    if(!ReadInode(InodeNum, &inode)){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: Read - Failed to read inode %u\n", InodeNum);
        return 0;
    }

    if(file->CurrentPosition >= inode.i_size){
        return 0; // EOF
    }

    U32 BytesToEnd = file->FileSize - file->CurrentPosition;
    if(size > BytesToEnd){
        size = BytesToEnd;
    }

    if(size == 0) return 0; // asumsi file rusak atau EOF

    U32 TotalBytesRead = 0;
    PageAlloc::DMAAlloc::DMABuffer* block_buffer = nullptr;

    while(size > 0){
        U32 FileBlockIndex = file->CurrentPosition / m_BlockSize;
        U32 OffsetInBlock = file->CurrentPosition % m_BlockSize;

        U32 DiskBlockNum = GetBlockNumForFileOffset(&inode, FileBlockIndex);
        if(DiskBlockNum == 0){
            Printk::Write(Printk::Level::LOG_ERR, "EXT2: Read - Invalid block number for file offset.\n");
            break;
        }

        U32 BytesFromThisBlock = m_BlockSize - OffsetInBlock;
        if(BytesFromThisBlock > size){
            BytesFromThisBlock = size;
        }

        if(!ReadBlock(DiskBlockNum, &block_buffer)){
            Printk::Write(Printk::Level::LOG_ERR, "EXT2: Read - Failed to read block %u\n", DiskBlockNum);
            break;
        }

        String::Memcpy(buffer + TotalBytesRead, 
            (U8*)block_buffer->VirtAddr + OffsetInBlock, BytesFromThisBlock);

        PageAlloc::DMAAlloc::FreeDMABuffer(block_buffer);
        block_buffer = nullptr;

        file->CurrentPosition += BytesFromThisBlock;
        TotalBytesRead += BytesFromThisBlock;
        size -= BytesFromThisBlock;

        if(file->CurrentPosition >= inode.i_size){
            break; // EOF
        }
    }

    return TotalBytesRead;

}

U32 EXT2FileSystem::Write(File *File, U8 *Buffer, U32 Size) {
    if(!File || !Buffer || Size == 0) return 0;
    if(File->IsDirectory){
        Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Write() called on a directory.\n");
        return 0;
    }

    U32 InodeNum = File->Internal_StartCluster;
    EXT2::Inode inode;
    if(!ReadInode(InodeNum, &inode)){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: Write - Failed to read inode %u\n", InodeNum);
        return 0;
    }

    U32 totalWritten = 0;
    U32 remaining = Size;

    while(remaining > 0){
        U32 fileBlockIndex = File->CurrentPosition / m_BlockSize;
        U32 offsetInBlock = File->CurrentPosition % m_BlockSize;

        U32 targetBlock = GetOrAllocateBlockForFileOffset(&inode, fileBlockIndex);
        if(targetBlock == 0){
            Printk::Write(Printk::Level::LOG_ERR, "EXT2: Write - Failed to get/allocate block for file block %u\n", fileBlockIndex);
            break;
        }

        PageAlloc::DMAAlloc::DMABuffer* blockBuf = nullptr;
        if(!ReadBlock(targetBlock, &blockBuf)){
            Printk::Write(Printk::Level::LOG_ERR, "EXT2: Write - Failed to read block %u\n", targetBlock);
            break;
        }

        U32 canWrite = m_BlockSize - offsetInBlock;
        if(canWrite > remaining) canWrite = remaining;

        // copy data into DMA buffer
        String::Memcpy((U8*)blockBuf->VirtAddr + offsetInBlock, Buffer + totalWritten, canWrite);

        if(!WriteBlock(targetBlock, blockBuf)){
            Printk::Write(Printk::Level::LOG_ERR, "EXT2: Write - Failed to write block %u\n", targetBlock);
            PageAlloc::DMAAlloc::FreeDMABuffer(blockBuf);
            break;
        }

        PageAlloc::DMAAlloc::FreeDMABuffer(blockBuf);

        File->CurrentPosition += canWrite;
        totalWritten += canWrite;
        remaining -= canWrite;

        if(File->CurrentPosition > inode.i_size){
            inode.i_size = File->CurrentPosition;
        }
    }

    // write inode back to disk
    if(!WriteInode(InodeNum, &inode)){
        Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Write - Failed to persist inode %u\n", InodeNum);
    }

    // Persist BGDT (block allocations changed BGDT counters)
    U32 BGDTStartBlocks = (m_BlockSize == 1024) ? 2 : 1;
    for(U32 i = 0; i < m_BGDT_SizeInBlocks; i++){
        PageAlloc::DMAAlloc::DMABuffer *tmp = PageAlloc::DMAAlloc::AllocateDMABytes(m_BlockSize);
        if(!tmp){
            Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Write - failed allocate DMABuffer to write BGDT block %u\n", i);
            break;
        }
        U8 *src = ((U8*)m_BGDT) + (i * m_BlockSize);
        String::Memcpy((U8*)tmp->VirtAddr, src, m_BlockSize);
        if(!WriteBlock(BGDTStartBlocks + i, tmp)){
            Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Write - failed write BGDT block %u\n", BGDTStartBlocks + i);
        }
        PageAlloc::DMAAlloc::FreeDMABuffer(tmp);
    }

    File->FileSize = inode.i_size;
    return totalWritten;
}

BOOL EXT2FileSystem::Delete(const char* path) {
    if(!path || String::Strlen(path) == 0) return FALSE;

    CHAR8 ParentPath[256];
    CHAR8 FileName[256];
    ParsePath((const CHAR8*)path, ParentPath, FileName);

    EXT2::Inode parentInode;
    U32 parentInodeNum = FindInodeForPath((const char*)ParentPath, &parentInode);
    if(parentInodeNum == 0){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: Delete - parent path not found: %s\n", ParentPath);
        return FALSE;
    }

    EXT2::DirectoryEntry dirent;
    if(!FindEntryInDirectory(&parentInode, (const CHAR8*)FileName, &dirent)){
        Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Delete - entry not found: %s\n", path);
        return FALSE;
    }

    U32 targetInodeNum = dirent.inode;
    EXT2::Inode targetInode;
    if(!ReadInode(targetInodeNum, &targetInode)){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: Delete - failed to read inode %u\n", targetInodeNum);
        return FALSE;
    }

    // If it's a directory, ensure it's empty (only . and ..)
    if(targetInode.i_mode & EXT2_S_IFDIR){
        // Simple check: scan direct blocks for any entry other than . and ..
        PageAlloc::DMAAlloc::DMABuffer* dbuf = nullptr;
        for(int i = 0; i < 12 && targetInode.i_block[i] != 0; i++){
            if(!ReadBlock(targetInode.i_block[i], &dbuf)) continue;
            U8* p = (U8*)dbuf->VirtAddr;
                U32 off = 0;
                while(off < m_BlockSize){
                    struct DirentHeader { U32 inode; U16 rec_len; U8 name_len; U8 file_type; } hdr;
                    String::Memcpy(&hdr, p + off, sizeof(hdr));
                    if(hdr.rec_len == 0) break;
                    if(hdr.inode != 0){
                        // copy name into temporary buffer
                        CHAR8 namebuf[256];
                        U32 namelen = hdr.name_len;
                        if(namelen > 255) namelen = 255;
                        String::Memcpy(namebuf, p + off + sizeof(hdr), namelen);
                        namebuf[namelen] = '\0';

                        // compare names
                        if(!ext2_dirent_name_compare(namebuf, hdr.name_len, ".") &&
                           !ext2_dirent_name_compare(namebuf, hdr.name_len, "..")){
                            PageAlloc::DMAAlloc::FreeDMABuffer(dbuf);
                            Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Delete - directory not empty: %s\n", path);
                            return FALSE;
                        }
                    }
                    off += hdr.rec_len;
                    if(hdr.rec_len == 0) break;
                }
            PageAlloc::DMAAlloc::FreeDMABuffer(dbuf);
            dbuf = nullptr;
        }
    }

    // Remove directory entry from parent
    if(!RemoveEntryFromDirectory(&parentInode, (const CHAR8*)FileName)) return FALSE;

    // Decrement link count (removing old link)
    if(targetInode.i_links_count > 0) targetInode.i_links_count--;
    if(!WriteInode(targetInodeNum, &targetInode)){
        Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Delete - failed to write inode %u after unlink\n", targetInodeNum);
    }



    // If link count reached zero, free data blocks and inode bitmaps
    if(targetInode.i_links_count == 0){
        // Free direct blocks
        for(int i = 0; i < 12; i++){
            U32 b = targetInode.i_block[i];
            if(b != 0){
                FreeBlock(b);
                targetInode.i_block[i] = 0;
            }
        }

        U32 EntriesPerBlock = m_BlockSize / sizeof(U32);

        // Single indirect
        if(targetInode.i_block[12] != 0){
            PageAlloc::DMAAlloc::DMABuffer* ibuf = nullptr;
            if(ReadBlock(targetInode.i_block[12], &ibuf)){
                U32* ptrs = (U32*)ibuf->VirtAddr;
                for(U32 j = 0; j < EntriesPerBlock; j++){
                    if(ptrs[j] != 0){
                        FreeBlock(ptrs[j]);
                        ptrs[j] = 0;
                    }
                }
                PageAlloc::DMAAlloc::FreeDMABuffer(ibuf);
            }
            // free the indirect block itself
            FreeBlock(targetInode.i_block[12]);
            targetInode.i_block[12] = 0;
        }

        // Double indirect
        if(targetInode.i_block[13] != 0){
            PageAlloc::DMAAlloc::DMABuffer* dbuf = nullptr;
            if(ReadBlock(targetInode.i_block[13], &dbuf)){
                U32* lvl1 = (U32*)dbuf->VirtAddr;
                for(U32 i1 = 0; i1 < EntriesPerBlock; i1++){
                    U32 first = lvl1[i1];
                    if(first == 0) continue;

                    PageAlloc::DMAAlloc::DMABuffer* sbuf = nullptr;
                    if(ReadBlock(first, &sbuf)){
                        U32* lvl2 = (U32*)sbuf->VirtAddr;
                        for(U32 j = 0; j < EntriesPerBlock; j++){
                            if(lvl2[j] != 0){
                                FreeBlock(lvl2[j]);
                                lvl2[j] = 0;
                            }
                        }
                        PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);
                    }

                    // free the single-indirect block
                    FreeBlock(first);
                    lvl1[i1] = 0;
                }
                PageAlloc::DMAAlloc::FreeDMABuffer(dbuf);
            }
            // free the double-indirect block itself
            FreeBlock(targetInode.i_block[13]);
            targetInode.i_block[13] = 0;
        }

        // Triple indirect
        if(targetInode.i_block[14] != 0){
            PageAlloc::DMAAlloc::DMABuffer* tbuf = nullptr;
            if(ReadBlock(targetInode.i_block[14], &tbuf)){
                U32* lvl1 = (U32*)tbuf->VirtAddr;
                for(U32 i1 = 0; i1 < EntriesPerBlock; i1++){
                    U32 lvl2_block = lvl1[i1];
                    if(lvl2_block == 0) continue;

                    PageAlloc::DMAAlloc::DMABuffer* mbuf = nullptr;
                    if(ReadBlock(lvl2_block, &mbuf)){
                        U32* lvl2 = (U32*)mbuf->VirtAddr;
                        for(U32 i2 = 0; i2 < EntriesPerBlock; i2++){
                            if(lvl2[i2] == 0) continue;
                            PageAlloc::DMAAlloc::DMABuffer* sbuf = nullptr;
                            if(ReadBlock(lvl2[i2], &sbuf)){
                                U32* ptrs = (U32*)sbuf->VirtAddr;
                                for(U32 j = 0; j < EntriesPerBlock; j++){
                                    if(ptrs[j] != 0){
                                        FreeBlock(ptrs[j]);
                                        ptrs[j] = 0;
                                    }
                                }
                                PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);
                            }

                            // free the single-indirect block
                            FreeBlock(lvl2[i2]);
                            lvl2[i2] = 0;
                        }
                        PageAlloc::DMAAlloc::FreeDMABuffer(mbuf);
                    }

                    // free the double-indirect block
                    FreeBlock(lvl2_block);
                    lvl1[i1] = 0;
                }
                PageAlloc::DMAAlloc::FreeDMABuffer(tbuf);
            }
            // free triple indirect block itself
            FreeBlock(targetInode.i_block[14]);
            targetInode.i_block[14] = 0;
        }

        targetInode.i_size = 0;
        targetInode.i_blocks = 0;

        // Clear inode bitmap and zero inode table entry
        if(!FreeInode(targetInodeNum)){
            Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Delete - failed to free inode %u\n", targetInodeNum);
        }

        // Persist BGDT and superblock counts
        PersistSuperblockAndBGDT();
    } else {
        // Still has links; just write updated inode
        if(!WriteInode(targetInodeNum, &targetInode)){
            Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Delete - failed to write inode %u\n", targetInodeNum);
        }
    }

    // Persist parent inode changes
    if(!WriteInode(parentInodeNum, &parentInode)){
        Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Delete - failed to write parent inode %u\n", parentInodeNum);
    }

    return TRUE;
}

BOOL EXT2FileSystem::Rename(const char* oldPath, const char* newPath) {
    if(!oldPath || !newPath) return FALSE;
    if(String::Strcmp(oldPath, newPath) == 0) return TRUE; // nothing to do

    CHAR8 OldParent[256], OldName[256];
    CHAR8 NewParent[256], NewName[256];
    ParsePath((const CHAR8*)oldPath, OldParent, OldName);
    ParsePath((const CHAR8*)newPath, NewParent, NewName);

    EXT2::Inode oldParentInode;
    U32 oldParentInodeNum = FindInodeForPath((const char*)OldParent, &oldParentInode);
    if(oldParentInodeNum == 0){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: Rename - old parent not found: %s\n", OldParent);
        return FALSE;
    }

    EXT2::DirectoryEntry oldDirent;
    if(!FindEntryInDirectory(&oldParentInode, (const CHAR8*)OldName, &oldDirent)){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: Rename - old entry not found: %s\n", oldPath);
        return FALSE;
    }

    U32 targetInodeNum = oldDirent.inode;
    EXT2::Inode targetInode;
    if(!ReadInode(targetInodeNum, &targetInode)){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: Rename - failed to read inode %u\n", targetInodeNum);
        return FALSE;
    }

    // Find new parent
    EXT2::Inode newParentInode;
    U32 newParentInodeNum = FindInodeForPath((const char*)NewParent, &newParentInode);
    if(newParentInodeNum == 0){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: Rename - new parent not found: %s\n", NewParent);
        return FALSE;
    }

    // Check if destination exists
    EXT2::DirectoryEntry existing;
    if(FindEntryInDirectory(&newParentInode, (const CHAR8*)NewName, &existing)){
        Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Rename - destination already exists: %s/%s\n", NewParent, NewName);
        return FALSE;
    }

    // Add entry to new parent
    if(!AddEntryToDirectory(&newParentInode, targetInodeNum, (const CHAR8*)NewName, (targetInode.i_mode & EXT2_S_IFDIR) ? EXT2_S_IFDIR : 0, newParentInodeNum)){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: Rename - failed to add entry to new parent %s/%s\n", NewParent, NewName);
        return FALSE;
    }

    // Update target inode link count (new link)
    targetInode.i_links_count++;
    if(!WriteInode(targetInodeNum, &targetInode)){
        Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Rename - failed to update inode links for %u\n", targetInodeNum);
    }

    // Remove old entry from old parent
    if(!RemoveEntryFromDirectory(&oldParentInode, (const CHAR8*)OldName)){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: Rename - failed to remove old entry %s\n", oldPath);
        // Note: we leave the new entry in place; report failure
        return FALSE;
    }

    // Decrement link count (removing old link)
    if(targetInode.i_links_count > 0) targetInode.i_links_count--;
    if(!WriteInode(targetInodeNum, &targetInode)){
        Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Rename - failed to write inode %u after unlink\n", targetInodeNum);
    }

    // Persist parent inodes
    if(!WriteInode(oldParentInodeNum, &oldParentInode)){
        Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Rename - failed to write old parent inode %u\n", oldParentInodeNum);
    }
    if(!WriteInode(newParentInodeNum, &newParentInode)){
        Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Rename - failed to write new parent inode %u\n", newParentInodeNum);
    }

    return TRUE;
}

BOOL EXT2FileSystem::Seek(File* file, U64 position) {
    if(!file) return FALSE; 
    if(file->IsDirectory){
        Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Seek() called on a directory.\n");
        return FALSE;
    }
    if(position > file->FileSize) return FALSE;

    if(position > 0xFFFFFFFF){
        Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Seek() position out of range (>4GB).\n");
        return FALSE;
    } else {
        file->CurrentPosition = (U32)position;
    }

    return TRUE;
}

//
//
// Fungsi Fungsi Helper
//
//

static VOID ParsePath(const CHAR8 *FullPath, CHAR8 *OutParentPath, CHAR8 *OutFileName){
    VAL32 Last_Slash_IDX = -1;
    for(VAL32 i = 0; FullPath[i] != '\0'; i++){
        if(FullPath[i] == '/'){
            Last_Slash_IDX = i;
        }
    }

    if(Last_Slash_IDX == -1){
        String::Strcpy(OutParentPath, "/");
        String::Strcpy(OutFileName, FullPath);
    } else if (Last_Slash_IDX == 0){
        String::Strcpy(OutParentPath, "/");
        String::Strcpy(OutFileName, &FullPath[1]);
    } else {
        String::Memcpy(OutParentPath, FullPath, Last_Slash_IDX);
        OutParentPath[Last_Slash_IDX] = '\0';
        String::Strcpy(OutFileName, &FullPath[Last_Slash_IDX + 1]);
    }
}

static BOOL ext2_dirent_name_compare(const char* name1, U8 len1, const char* name2){
    if(!name1 || !name2) return FALSE;
    U32 l2 = String::Strlen(name2);
    if((U32)len1 != l2) return FALSE;
    return (String::Memcmp(name1, name2, len1) == 0) ? TRUE : FALSE;
}

BOOL EXT2FileSystem::FindEntryInDirectory(EXT2::Inode *DirInode, const CHAR8 *FileName, EXT2::DirectoryEntry *OutEntry){
    if(!(DirInode->i_mode & EXT2_S_IFDIR)) return FALSE;

    PageAlloc::DMAAlloc::DMABuffer *DirDataBuffer = nullptr;

    
    // helper to scan a single directory data block
    auto scan_dir_block = [&](U32 blockNum)->BOOL{
        if(blockNum == 0) return FALSE;
        if(!ReadBlock(blockNum, &DirDataBuffer)) return FALSE;
        U8 *DirPtr = (U8*)DirDataBuffer->VirtAddr;
        U32 Offset = 0;
        struct DirentHeader { U32 inode; U16 rec_len; U8 name_len; U8 file_type; } hdr;
        while(Offset < m_BlockSize){
            U8* entryPtr = DirPtr + Offset;
            String::Memcpy(&hdr, entryPtr, sizeof(hdr));
            if(hdr.rec_len == 0) break;
            if(hdr.inode == 0){ Offset += hdr.rec_len; continue; }

            CHAR8 namebuf[256];
            U32 namelen = hdr.name_len;
            if(namelen > 255) namelen = 255;
            String::Memcpy(namebuf, entryPtr + sizeof(hdr), namelen);
            namebuf[namelen] = '\0';

            if(ext2_dirent_name_compare(namebuf, hdr.name_len, FileName)){
                if(OutEntry){
                    U32 copyLen = hdr.rec_len;
                    if(copyLen > sizeof(EXT2::DirectoryEntry)) copyLen = sizeof(EXT2::DirectoryEntry);
                    String::Memset((U8*)OutEntry, 0, sizeof(EXT2::DirectoryEntry));
                    String::Memcpy((U8*)OutEntry, entryPtr, copyLen);
                }
                PageAlloc::DMAAlloc::FreeDMABuffer(DirDataBuffer);
                return TRUE;
            }
            Offset += hdr.rec_len;
        }
        PageAlloc::DMAAlloc::FreeDMABuffer(DirDataBuffer);
        return FALSE;
    };

    // 1) scan direct blocks
    for(int i = 0; i < 12; i++){
        if(DirInode->i_block[i] == 0) continue;
        if(scan_dir_block(DirInode->i_block[i])) return TRUE;
    }

    U32 EntriesPerBlock = m_BlockSize / sizeof(U32);

    // 2) single indirect
    if(DirInode->i_block[12] != 0){
        PageAlloc::DMAAlloc::DMABuffer* ibuf = nullptr;
        if(ReadBlock(DirInode->i_block[12], &ibuf)){
            U32* ptrs = (U32*)ibuf->VirtAddr;
            for(U32 j = 0; j < EntriesPerBlock; j++){
                if(ptrs[j] == 0) continue;
                if(scan_dir_block(ptrs[j])){ PageAlloc::DMAAlloc::FreeDMABuffer(ibuf); return TRUE; }
            }
            PageAlloc::DMAAlloc::FreeDMABuffer(ibuf);
        }
    }

    // 3) double indirect
    if(DirInode->i_block[13] != 0){
        PageAlloc::DMAAlloc::DMABuffer* dbuf = nullptr;
        if(ReadBlock(DirInode->i_block[13], &dbuf)){
            U32* lvl1 = (U32*)dbuf->VirtAddr;
            for(U32 i1 = 0; i1 < EntriesPerBlock; i1++){
                if(lvl1[i1] == 0) continue;
                PageAlloc::DMAAlloc::DMABuffer* sbuf = nullptr;
                if(ReadBlock(lvl1[i1], &sbuf)){
                    U32* ptrs = (U32*)sbuf->VirtAddr;
                    for(U32 j = 0; j < EntriesPerBlock; j++){
                        if(ptrs[j] == 0) continue;
                        if(scan_dir_block(ptrs[j])){ PageAlloc::DMAAlloc::FreeDMABuffer(sbuf); PageAlloc::DMAAlloc::FreeDMABuffer(dbuf); return TRUE; }
                    }
                    PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);
                }
            }
            PageAlloc::DMAAlloc::FreeDMABuffer(dbuf);
        }
    }

    // 4) triple indirect
    if(DirInode->i_block[14] != 0){
        PageAlloc::DMAAlloc::DMABuffer* tbuf = nullptr;
        if(ReadBlock(DirInode->i_block[14], &tbuf)){
            U32* lvl1 = (U32*)tbuf->VirtAddr;
            for(U32 i1 = 0; i1 < EntriesPerBlock; i1++){
                if(lvl1[i1] == 0) continue;
                PageAlloc::DMAAlloc::DMABuffer* mbuf = nullptr;
                if(ReadBlock(lvl1[i1], &mbuf)){
                    U32* lvl2 = (U32*)mbuf->VirtAddr;
                    for(U32 i2 = 0; i2 < EntriesPerBlock; i2++){
                        if(lvl2[i2] == 0) continue;
                        PageAlloc::DMAAlloc::DMABuffer* sbuf = nullptr;
                        if(ReadBlock(lvl2[i2], &sbuf)){
                            U32* ptrs = (U32*)sbuf->VirtAddr;
                            for(U32 j = 0; j < EntriesPerBlock; j++){
                                if(ptrs[j] == 0) continue;
                                if(scan_dir_block(ptrs[j])){ PageAlloc::DMAAlloc::FreeDMABuffer(sbuf); PageAlloc::DMAAlloc::FreeDMABuffer(mbuf); PageAlloc::DMAAlloc::FreeDMABuffer(tbuf); return TRUE; }
                            }
                            PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);
                        }
                    }
                    PageAlloc::DMAAlloc::FreeDMABuffer(mbuf);
                }
            }
            PageAlloc::DMAAlloc::FreeDMABuffer(tbuf);
        }
    }

    return FALSE;
}

BOOL EXT2FileSystem::RemoveEntryFromDirectory(EXT2::Inode *DirInode, const CHAR8* Name){
    if(!(DirInode->i_mode & EXT2_S_IFDIR)) return FALSE;

    PageAlloc::DMAAlloc::DMABuffer *DirDataBuffer = nullptr;

    auto remove_in_block = [&](U32 BlockNum)->BOOL{
        if(BlockNum == 0) return FALSE;
        if(!ReadBlock(BlockNum, &DirDataBuffer)) return FALSE;
        U8 *DirPtr = (U8*)DirDataBuffer->VirtAddr;
        U32 Offset = 0;
        while(Offset < m_BlockSize){
            U8* entryPtr = DirPtr + Offset;
            struct DirentHeader { U32 inode; U16 rec_len; U8 name_len; U8 file_type; } hdr;
            String::Memcpy(&hdr, entryPtr, sizeof(hdr));
            if(hdr.rec_len == 0) break;

            CHAR8 namebuf[256];
            U32 namelen = hdr.name_len;
            if(namelen > 255) namelen = 255;
            String::Memcpy(namebuf, entryPtr + sizeof(hdr), namelen);
            namebuf[namelen] = '\0';

            if(hdr.inode != 0 && ext2_dirent_name_compare(namebuf, hdr.name_len, Name)){
                struct DirentHeader newhdr = hdr;
                newhdr.inode = 0;
                newhdr.name_len = 0;
                newhdr.file_type = 0;
                String::Memcpy(entryPtr, &newhdr, sizeof(newhdr));

                if(!WriteBlock(BlockNum, DirDataBuffer)){
                    PageAlloc::DMAAlloc::FreeDMABuffer(DirDataBuffer);
                    return FALSE;
                }
                PageAlloc::DMAAlloc::FreeDMABuffer(DirDataBuffer);
                return TRUE;
            }

            Offset += hdr.rec_len;
        }
        PageAlloc::DMAAlloc::FreeDMABuffer(DirDataBuffer);
        return FALSE;
    };

    // direct
    for(int i = 0; i < 12; i++){
        if(DirInode->i_block[i] == 0) continue;
        if(remove_in_block(DirInode->i_block[i])) return TRUE;
    }

    U32 EntriesPerBlock = m_BlockSize / sizeof(U32);

    // single indirect
    if(DirInode->i_block[12] != 0){
        PageAlloc::DMAAlloc::DMABuffer* ibuf = nullptr;
        if(ReadBlock(DirInode->i_block[12], &ibuf)){
            U32* ptrs = (U32*)ibuf->VirtAddr;
            for(U32 j = 0; j < EntriesPerBlock; j++){
                if(ptrs[j] == 0) continue;
                if(remove_in_block(ptrs[j])){ PageAlloc::DMAAlloc::FreeDMABuffer(ibuf); return TRUE; }
            }
            PageAlloc::DMAAlloc::FreeDMABuffer(ibuf);
        }
    }

    // double indirect
    if(DirInode->i_block[13] !=  0){
        PageAlloc::DMAAlloc::DMABuffer* dbuf = nullptr;
        if(ReadBlock(DirInode->i_block[13], &dbuf)){
            U32* lvl1 = (U32*)dbuf->VirtAddr;
            for(U32 i1 = 0; i1 < EntriesPerBlock; i1++){
                if(lvl1[i1] == 0) continue;
                PageAlloc::DMAAlloc::DMABuffer* sbuf = nullptr;
                if(ReadBlock(lvl1[i1], &sbuf)){
                    U32* ptrs = (U32*)sbuf->VirtAddr;
                    for(U32 j = 0; j < EntriesPerBlock; j++){
                        if(ptrs[j] == 0) continue;
                        if(remove_in_block(ptrs[j])){ PageAlloc::DMAAlloc::FreeDMABuffer(sbuf); PageAlloc::DMAAlloc::FreeDMABuffer(dbuf); return TRUE; }
                    }
                    PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);
                }
            }
            PageAlloc::DMAAlloc::FreeDMABuffer(dbuf);
        }
    }

    // triple indirect
    if(DirInode->i_block[14] != 0){
        PageAlloc::DMAAlloc::DMABuffer* tbuf = nullptr;
        if(ReadBlock(DirInode->i_block[14], &tbuf)){
            U32* lvl1 = (U32*)tbuf->VirtAddr;
            for(U32 i1 = 0; i1 < EntriesPerBlock; i1++){
                if(lvl1[i1] == 0) continue;
                PageAlloc::DMAAlloc::DMABuffer* mbuf = nullptr;
                if(ReadBlock(lvl1[i1], &mbuf)){
                    U32* lvl2 = (U32*)mbuf->VirtAddr;
                    for(U32 i2 = 0; i2 < EntriesPerBlock; i2++){
                        if(lvl2[i2] == 0) continue;
                        PageAlloc::DMAAlloc::DMABuffer* sbuf = nullptr;
                        if(ReadBlock(lvl2[i2], &sbuf)){
                            U32* ptrs = (U32*)sbuf->VirtAddr;
                            for(U32 j = 0; j < EntriesPerBlock; j++){
                                if(ptrs[j] == 0) continue;
                                if(remove_in_block(ptrs[j])){ PageAlloc::DMAAlloc::FreeDMABuffer(sbuf); PageAlloc::DMAAlloc::FreeDMABuffer(mbuf); PageAlloc::DMAAlloc::FreeDMABuffer(tbuf); return TRUE; }
                            }
                            PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);
                        }
                    }
                    PageAlloc::DMAAlloc::FreeDMABuffer(mbuf);
                }
            }
            PageAlloc::DMAAlloc::FreeDMABuffer(tbuf);
        }
    }

    return FALSE;
}

static U16 GetNeedRecLen(U8 NameLen){
    U16 Len = 8 + NameLen;
    return (Len + 3) & ~3;
}

BOOL EXT2FileSystem::AddEntryToDirectory(EXT2::Inode *DirInode, U32 NewInodeNum, const CHAR8* NewFileName, U16 FileType, U32 ParentInodeNum){
    if(!(DirInode->i_mode & EXT2_S_IFDIR)) return FALSE;

    U8 NewNameLen = (U8)String::Strlen(NewFileName);
    U16 NewNeededLen = GetNeedRecLen(NewNameLen);

    PageAlloc::DMAAlloc::DMABuffer *DirDataBuffer = nullptr;

    // Allocate a reusable zeroed DMA buffer to reduce frequent kmalloc churn
    // when initializing newly allocated blocks. We free it on all exits.
    PageAlloc::DMAAlloc::DMABuffer* zeroBuf = PageAlloc::DMAAlloc::AllocateDMABytes(m_BlockSize);
    if(zeroBuf){ String::Memset((void*)zeroBuf->VirtAddr, 0, m_BlockSize); }
    auto ret_bool = [&](BOOL v)->BOOL{ if(zeroBuf) PageAlloc::DMAAlloc::FreeDMABuffer(zeroBuf); return v; };

    // Track direct blocks we allocate here so we can rollback on failures.
    U32 allocated_direct_indices[12];
    U32 allocated_direct_count = 0;
    for(int _ai = 0; _ai < 12; _ai++) allocated_direct_indices[_ai] = 0;

    auto cleanup_allocated_directs = [&](){
        // free any direct blocks we allocated during this insertion attempt
        for(U32 _k = 0; _k < allocated_direct_count; _k++){
            int di = (int)allocated_direct_indices[_k];
            if(di < 0 || di >= 12) continue;
            U32 bn = DirInode->i_block[di];
            if(bn != 0){
                DirInode->i_block[di] = 0;
                DirInode->i_blocks -= (m_BlockSize / 512);
                FreeBlock(bn);
            }
        }
        // best-effort persist cleared parent inode if requested
        if(ParentInodeNum != 0){
            WriteInode(ParentInodeNum, DirInode);
        }
    };

    auto try_add_in_block = [&](U32 BlockNum)->BOOL{
        if(BlockNum == 0) return FALSE;
        if(!ReadBlock(BlockNum, &DirDataBuffer)) return FALSE;
        U8 *DirPtr = (U8*)DirDataBuffer->VirtAddr;
        U32 Offset = 0;
        struct DirentHeader { U32 inode; U16 rec_len; U8 name_len; U8 file_type; } hdr;
        while(Offset < m_BlockSize){
            U8* entryPtr = DirPtr + Offset;
            String::Memcpy(&hdr, entryPtr, sizeof(hdr));
            if(hdr.rec_len == 0) break;

            if(Offset + hdr.rec_len >= m_BlockSize){
                U16 LastNeededLen = (hdr.inode == 0) ? 0 : GetNeedRecLen(hdr.name_len);
                U16 FreeSpace = hdr.rec_len - LastNeededLen;

                if(FreeSpace >= NewNeededLen){
                    if(hdr.inode != 0){
                        U16 existing_rec_len = LastNeededLen;
                        String::Memcpy(entryPtr + offsetof(EXT2::DirectoryEntry, rec_len), &existing_rec_len, sizeof(existing_rec_len));
                    }

                    U8* newEntryPtr = DirPtr + Offset + LastNeededLen;
                    struct NewDirentHdr { U32 inode; U16 rec_len; U8 name_len; U8 file_type; } newhdr;
                    newhdr.inode = NewInodeNum;
                    newhdr.rec_len = FreeSpace;
                    newhdr.name_len = NewNameLen;
                    newhdr.file_type = (FileType & EXT2_S_IFDIR) ? 2 : 1;
                    String::Memcpy(newEntryPtr, &newhdr, sizeof(newhdr));
                    String::Memcpy(newEntryPtr + sizeof(newhdr), NewFileName, NewNameLen);

                    if(!WriteBlock(BlockNum, DirDataBuffer)){
                        PageAlloc::DMAAlloc::FreeDMABuffer(DirDataBuffer);
                        return FALSE;
                    }
                    PageAlloc::DMAAlloc::FreeDMABuffer(DirDataBuffer);
                    return TRUE;
                }
            }
            Offset += hdr.rec_len;
        }
        PageAlloc::DMAAlloc::FreeDMABuffer(DirDataBuffer);
        return FALSE;
    };

    // 1) try direct blocks; allocate empty block when pointer is 0
    for(int i = 0; i < 12; i++){
        U32 BlockNum = DirInode->i_block[i];
        if(BlockNum == 0){
            BlockNum = AllocateBlock();
            if(BlockNum == 0) { cleanup_allocated_directs(); return ret_bool(FALSE); }
            DirInode->i_block[i] = BlockNum;
            // record this allocation so we can undo it on later failure
            if(allocated_direct_count < 12) allocated_direct_indices[allocated_direct_count++] = i;
            // initialize empty block (reuse zeroBuf to avoid frequent alloc/free)
            if(zeroBuf){ WriteBlock(BlockNum, zeroBuf); }
            else {
                PageAlloc::DMAAlloc::DMABuffer* z = PageAlloc::DMAAlloc::AllocateDMABytes(m_BlockSize);
                if(z){ String::Memset((void*)z->VirtAddr, 0, m_BlockSize); WriteBlock(BlockNum, z); PageAlloc::DMAAlloc::FreeDMABuffer(z); }
            }
            DirInode->i_blocks += (m_BlockSize / 512);
        }
        if(try_add_in_block(BlockNum)){
            if(ParentInodeNum != 0) WriteInode(ParentInodeNum, DirInode);
            return ret_bool(TRUE);
        }
    }

    U32 EntriesPerBlock = m_BlockSize / sizeof(U32);

    // 2) single indirect: ensure indirect block exists, then scan/allocate blocks as needed
    if(DirInode->i_block[12] == 0){
        U32 ib = AllocateBlock();
        if(ib == 0) { /* can't create indirect, skip */ }
        else {
            DirInode->i_block[12] = ib;
            if(zeroBuf){ WriteBlock(ib, zeroBuf); }
            else { PageAlloc::DMAAlloc::DMABuffer* z = PageAlloc::DMAAlloc::AllocateDMABytes(m_BlockSize); if(z){ String::Memset((void*)z->VirtAddr, 0, m_BlockSize); WriteBlock(ib, z); PageAlloc::DMAAlloc::FreeDMABuffer(z); } }
            DirInode->i_blocks += (m_BlockSize / 512);
        }
    }

    if(DirInode->i_block[12] != 0){
        PageAlloc::DMAAlloc::DMABuffer* ibuf = nullptr;
        if(ReadBlock(DirInode->i_block[12], &ibuf)){
            U32* ptrs = (U32*)ibuf->VirtAddr;
            for(U32 j = 0; j < EntriesPerBlock; j++){
                if(ptrs[j] == 0){
                    U32 nb = AllocateBlock();
                    if(nb == 0) continue;
                    // initialize new dir data block first (transactional: create data before publishing pointer)
                    if(zeroBuf){
                        if(!WriteBlock(nb, zeroBuf)){
                            FreeBlock(nb);
                            continue; // try next slot
                        }
                    } else {
                        PageAlloc::DMAAlloc::DMABuffer* z = PageAlloc::DMAAlloc::AllocateDMABytes(m_BlockSize);
                        if(z){
                            String::Memset((void*)z->VirtAddr, 0, m_BlockSize);
                            if(!WriteBlock(nb, z)){
                                PageAlloc::DMAAlloc::FreeDMABuffer(z);
                                FreeBlock(nb);
                                continue; // try next slot
                            }
                            PageAlloc::DMAAlloc::FreeDMABuffer(z);
                        }
                    }
                    // publish pointer in indirect block
                    ptrs[j] = nb;
                    if(!WriteBlock(DirInode->i_block[12], ibuf)){
                        // rollback: clear published pointer and free the allocated block
                        ptrs[j] = 0;
                        // best-effort: try to persist the cleared pointer
                        WriteBlock(DirInode->i_block[12], ibuf);
                        FreeBlock(nb);
                        PageAlloc::DMAAlloc::FreeDMABuffer(ibuf);
                        cleanup_allocated_directs();
                        return ret_bool(FALSE);
                    }
                    DirInode->i_blocks += (m_BlockSize / 512);
                }
                if(ptrs[j] != 0){
                    if(try_add_in_block(ptrs[j])){ PageAlloc::DMAAlloc::FreeDMABuffer(ibuf); if(ParentInodeNum != 0) WriteInode(ParentInodeNum, DirInode); return ret_bool(TRUE); }
                }
            }
            PageAlloc::DMAAlloc::FreeDMABuffer(ibuf);
        }
    }

    // 3) double indirect (lvl1 -> single-indirect -> data blocks)
    if(DirInode->i_block[13] == 0){
        U32 db = AllocateBlock();
    if(db != 0){
            DirInode->i_block[13] = db;
            if(zeroBuf){ WriteBlock(db, zeroBuf); }
            else { PageAlloc::DMAAlloc::DMABuffer* z = PageAlloc::DMAAlloc::AllocateDMABytes(m_BlockSize); if(z){ String::Memset((void*)z->VirtAddr,0,m_BlockSize); WriteBlock(db,z); PageAlloc::DMAAlloc::FreeDMABuffer(z); } }
            DirInode->i_blocks += (m_BlockSize/512);
        }
    }
    if(DirInode->i_block[13] != 0){
        PageAlloc::DMAAlloc::DMABuffer* dbuf = nullptr;
        if(ReadBlock(DirInode->i_block[13], &dbuf)){
            U32* lvl1 = (U32*)dbuf->VirtAddr; // pointers to single-indirect blocks
            for(U32 i1 = 0; i1 < EntriesPerBlock; i1++){
                // ensure single-indirect block exists; create and initialize before publishing its pointer
                if(lvl1[i1] == 0){
                    U32 sb = AllocateBlock();
                    if(sb == 0) continue;
                    if(zeroBuf){ WriteBlock(sb, zeroBuf); }
                    else { PageAlloc::DMAAlloc::DMABuffer* z = PageAlloc::DMAAlloc::AllocateDMABytes(m_BlockSize); if(z){ String::Memset((void*)z->VirtAddr,0,m_BlockSize); WriteBlock(sb,z); PageAlloc::DMAAlloc::FreeDMABuffer(z); } }
                    lvl1[i1] = sb;
                    if(!WriteBlock(DirInode->i_block[13], dbuf)){
                        // rollback: free single-indirect block we just allocated
                        lvl1[i1] = 0;
                        FreeBlock(sb);
                        PageAlloc::DMAAlloc::FreeDMABuffer(dbuf);
                        cleanup_allocated_directs();
                        return ret_bool(FALSE);
                    }
                    DirInode->i_blocks += (m_BlockSize/512);
                }

                // read the single-indirect block and iterate its data pointers
                PageAlloc::DMAAlloc::DMABuffer* sbuf = nullptr;
                if(!ReadBlock(lvl1[i1], &sbuf)) continue;
                U32* ptrs = (U32*)sbuf->VirtAddr;

                for(U32 j = 0; j < EntriesPerBlock; j++){
                    if(ptrs[j] == 0){
                        // allocate and initialize data block before publishing pointer
                        U32 nb = AllocateBlock();
                        if(nb == 0) continue;
                        if(zeroBuf){ WriteBlock(nb, zeroBuf); }
                        else { PageAlloc::DMAAlloc::DMABuffer* zdata = PageAlloc::DMAAlloc::AllocateDMABytes(m_BlockSize); if(zdata){ String::Memset((void*)zdata->VirtAddr,0,m_BlockSize); WriteBlock(nb,zdata); PageAlloc::DMAAlloc::FreeDMABuffer(zdata); } }
                        ptrs[j] = nb;
                        if(!WriteBlock(lvl1[i1], sbuf)){
                            // rollback: clear pointer and free allocated data block
                            ptrs[j] = 0;
                            // best-effort persist of cleared pointer
                            WriteBlock(lvl1[i1], sbuf);
                            FreeBlock(nb);
                            PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);
                            PageAlloc::DMAAlloc::FreeDMABuffer(dbuf);
                            cleanup_allocated_directs();
                            return ret_bool(FALSE);
                        }
                        DirInode->i_blocks += (m_BlockSize/512);
                    }

                    if(ptrs[j] != 0){
                        if(try_add_in_block(ptrs[j])){ PageAlloc::DMAAlloc::FreeDMABuffer(sbuf); PageAlloc::DMAAlloc::FreeDMABuffer(dbuf); if(ParentInodeNum != 0) WriteInode(ParentInodeNum, DirInode); return ret_bool(TRUE); }
                    }
                }

                PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);
            }
            PageAlloc::DMAAlloc::FreeDMABuffer(dbuf);
        }
    }

    // 4) triple indirect (lvl1 -> lvl2 -> single-indirect -> data blocks)
    if(DirInode->i_block[14] == 0){
        U32 tb = AllocateBlock();
        if(tb != 0){
            DirInode->i_block[14] = tb;
            PageAlloc::DMAAlloc::DMABuffer* z = PageAlloc::DMAAlloc::AllocateDMABytes(m_BlockSize);
            if(z){ String::Memset((void*)z->VirtAddr,0,m_BlockSize); WriteBlock(tb,z); PageAlloc::DMAAlloc::FreeDMABuffer(z); }
            DirInode->i_blocks += (m_BlockSize/512);
        }
    }
    if(DirInode->i_block[14] != 0){
        PageAlloc::DMAAlloc::DMABuffer* tbuf = nullptr;
        if(ReadBlock(DirInode->i_block[14], &tbuf)){
            U32* lvl1 = (U32*)tbuf->VirtAddr; // pointers to lvl2 blocks
            for(U32 i1 = 0; i1 < EntriesPerBlock; i1++){
                // ensure lvl2 block exists
                if(lvl1[i1] == 0){
                    U32 lvl2blk = AllocateBlock();
                    if(lvl2blk == 0) continue;
                    if(zeroBuf){ WriteBlock(lvl2blk, zeroBuf); }
                    else { PageAlloc::DMAAlloc::DMABuffer* z = PageAlloc::DMAAlloc::AllocateDMABytes(m_BlockSize); if(z){ String::Memset((void*)z->VirtAddr,0,m_BlockSize); WriteBlock(lvl2blk,z); PageAlloc::DMAAlloc::FreeDMABuffer(z); } }
                    lvl1[i1] = lvl2blk;
                    if(!WriteBlock(DirInode->i_block[14], tbuf)){
                        // rollback: free lvl2 block allocated
                        lvl1[i1] = 0;
                        FreeBlock(lvl2blk);
                        PageAlloc::DMAAlloc::FreeDMABuffer(tbuf);
                        cleanup_allocated_directs();
                        return ret_bool(FALSE);
                    }
                    DirInode->i_blocks += (m_BlockSize/512);
                }

                PageAlloc::DMAAlloc::DMABuffer* mbuf = nullptr;
                if(!ReadBlock(lvl1[i1], &mbuf)) continue;
                U32* lvl2 = (U32*)mbuf->VirtAddr; // pointers to single-indirect blocks

                for(U32 i2 = 0; i2 < EntriesPerBlock; i2++){
                    if(lvl2[i2] == 0){
                        U32 sb = AllocateBlock();
                        if(sb == 0) continue;
                        if(zeroBuf){ WriteBlock(sb, zeroBuf); }
                        else { PageAlloc::DMAAlloc::DMABuffer* z = PageAlloc::DMAAlloc::AllocateDMABytes(m_BlockSize); if(z){ String::Memset((void*)z->VirtAddr,0,m_BlockSize); WriteBlock(sb,z); PageAlloc::DMAAlloc::FreeDMABuffer(z); } }
                        lvl2[i2] = sb;
                        if(!WriteBlock(lvl1[i1], mbuf)){
                            // rollback: free newly allocated single-indirect block
                            lvl2[i2] = 0;
                            FreeBlock(sb);
                            PageAlloc::DMAAlloc::FreeDMABuffer(mbuf);
                            PageAlloc::DMAAlloc::FreeDMABuffer(tbuf);
                            cleanup_allocated_directs();
                            return ret_bool(FALSE);
                        }
                        DirInode->i_blocks += (m_BlockSize/512);
                    }

                    // read single-indirect block
                    PageAlloc::DMAAlloc::DMABuffer* sbuf = nullptr;
                    if(!ReadBlock(lvl2[i2], &sbuf)) continue;
                    U32* ptrs = (U32*)sbuf->VirtAddr;

                    for(U32 j = 0; j < EntriesPerBlock; j++){
                        if(ptrs[j] == 0){
                            U32 nb = AllocateBlock();
                            if(nb == 0) continue;
                            if(zeroBuf){ WriteBlock(nb, zeroBuf); }
                            else { PageAlloc::DMAAlloc::DMABuffer* zdata = PageAlloc::DMAAlloc::AllocateDMABytes(m_BlockSize); if(zdata){ String::Memset((void*)zdata->VirtAddr,0,m_BlockSize); WriteBlock(nb,zdata); PageAlloc::DMAAlloc::FreeDMABuffer(zdata); } }
                            ptrs[j] = nb;
                            if(!WriteBlock(lvl2[i2], sbuf)){
                                // rollback: clear pointer and free data block
                                ptrs[j] = 0;
                                // best-effort persist cleared pointer
                                WriteBlock(lvl2[i2], sbuf);
                                FreeBlock(nb);
                                PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);
                                PageAlloc::DMAAlloc::FreeDMABuffer(mbuf);
                                PageAlloc::DMAAlloc::FreeDMABuffer(tbuf);
                                cleanup_allocated_directs();
                                return ret_bool(FALSE);
                            }
                            DirInode->i_blocks += (m_BlockSize/512);
                        }

                        if(ptrs[j] != 0){
                            if(try_add_in_block(ptrs[j])){ PageAlloc::DMAAlloc::FreeDMABuffer(sbuf); PageAlloc::DMAAlloc::FreeDMABuffer(mbuf); PageAlloc::DMAAlloc::FreeDMABuffer(tbuf); if(ParentInodeNum != 0) WriteInode(ParentInodeNum, DirInode); return ret_bool(TRUE); }
                        }
                    }

                    PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);
                }

                PageAlloc::DMAAlloc::FreeDMABuffer(mbuf);
            }
            PageAlloc::DMAAlloc::FreeDMABuffer(tbuf);
        }
    }

    Printk::Write(Printk::Level::LOG_ERR, "EXT2: AddEntryToDirectory - No space to add new entry %s\n", NewFileName);
    return ret_bool(FALSE);
}

BOOL EXT2FileSystem::WriteInode(U32 InodeNum, EXT2::Inode *InodeBuffer){
    if(InodeNum == 0 || !InodeBuffer){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: WriteInode called with invalid parameters.\n");
        return FALSE;
    }

    U32 Group = (InodeNum - 1) / m_Superblock.s_inodes_per_group;
    if(Group >= m_NumBlockGroups){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: WriteInode - Inode number %u out of range (group %u >= %u)\n",
            InodeNum, Group, m_NumBlockGroups);
        return FALSE;
    }

    EXT2::BlockGroupDescriptor *BGD = &m_BGDT[Group];
    U32 InodeTableBlock = BGD->bg_inode_table;
    U32 Index = (InodeNum - 1) % m_Superblock.s_inodes_per_group;
    U16 InodeSize = m_Superblock.s_inode_size;
    U64 OffsetInTable = (U64)Index * InodeSize;
    U32 BlockOffset = OffsetInTable / m_BlockSize;
    U32 OffsetInBlock = OffsetInTable % m_BlockSize;
    U32 TargetBlock = InodeTableBlock + BlockOffset;

    PageAlloc::DMAAlloc::DMABuffer *Buffer = nullptr;
    if(!ReadBlock(TargetBlock, &Buffer)){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: WriteInode - Failed to read block %u for inode %u\n",
            TargetBlock, InodeNum);
        return FALSE;
    }

    String::Memcpy((U8*)Buffer->VirtAddr + OffsetInBlock, InodeBuffer, sizeof(EXT2::Inode));

    if(!WriteBlock(TargetBlock, Buffer)){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: WriteInode - Failed to write block %u for inode %u\n",
            TargetBlock, InodeNum);
        PageAlloc::DMAAlloc::FreeDMABuffer(Buffer);
        return FALSE;
    }

    PageAlloc::DMAAlloc::FreeDMABuffer(Buffer);
    return TRUE;
}

BOOL EXT2FileSystem::ReadInode(U32 inodeNum, EXT2::Inode* outInodeBuffer){
    if(inodeNum == 0 || !outInodeBuffer){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: ReadInode called with invalid parameters.\n");
        return FALSE;
    }

    U32 Group = (inodeNum - 1) / m_Superblock.s_inodes_per_group;
    if(Group >= m_NumBlockGroups){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: ReadInode - Inode number %u out of range (group %u >= %u)\n",
            inodeNum, Group, m_NumBlockGroups);
        return FALSE;
    }

    EXT2::BlockGroupDescriptor *BGD = &m_BGDT[Group];
    U32 InodeTableBlock = BGD->bg_inode_table;
    U32 Index = (inodeNum - 1) % m_Superblock.s_inodes_per_group;
    U16 InodeSize = m_Superblock.s_inode_size;
    U64 OffsetInTable = (U64)Index * InodeSize;
    U32 BlockOffset = OffsetInTable / m_BlockSize;
    U32 OffsetInBlock = OffsetInTable % m_BlockSize;
    U32 TargetBlock = InodeTableBlock + BlockOffset;

    PageAlloc::DMAAlloc::DMABuffer *Buffer = nullptr;
    if(!ReadBlock(TargetBlock, &Buffer)){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: ReadInode - Failed to read block %u for inode %u\n",
            TargetBlock, inodeNum);
        return FALSE;
    }

    // copy inode structure out
    String::Memcpy(outInodeBuffer, (U8*)Buffer->VirtAddr + OffsetInBlock, sizeof(EXT2::Inode));

    PageAlloc::DMAAlloc::FreeDMABuffer(Buffer);
    return TRUE;
}

// Debug helpers
void EXT2FileSystem::DebugSetFailAfter(U32 writes){
    m_DebugFailAfter = writes;
    m_DebugWriteCounter = 0;
}

void EXT2FileSystem::DebugResetFail(){
    m_DebugFailAfter = 0;
    m_DebugWriteCounter = 0;
}

void EXT2FileSystem::DebugSetDefaultOwner(U32 uid, U32 gid){
    m_DebugDefaultUid = uid;
    m_DebugDefaultGid = gid;
}

// Walk BGDT/bitmaps and inodes to detect mismatches (best-effort)
BOOL EXT2FileSystem::DebugConsistencyCheck(){
    Printk::Write(Printk::Level::LOG_DEBUG, "EXT2: DebugConsistencyCheck - begin\n");
    if(!m_Partition){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: DebugConsistencyCheck - not mounted\n");
        return FALSE;
    }

    U32 totalBlocks = m_Superblock.s_blocks_count;
    U32 totalInodes = m_Superblock.s_inodes_count;

    // Bitmap for blocks referenced by inodes
    U32 bmBytes = (totalBlocks + 7) / 8;
    U8* refBitmap = (U8*)Kmalloc::Alloc(bmBytes);
    if(!refBitmap){ Printk::Write(Printk::Level::LOG_ERR, "EXT2: DebugConsistencyCheck - alloc failed\n"); return FALSE; }
    String::Memset(refBitmap, 0, bmBytes);

    auto mark_block = [&](U32 b){ if(b == 0 || b > totalBlocks) return; U32 idx = b - 1; refBitmap[idx/8] |= (1 << (idx%8)); };

    // Walk all inodes and mark blocks they reference (direct + indirect up to triple)
    for(U32 ino = 1; ino <= totalInodes; ino++){
        EXT2::Inode inode;
        if(!ReadInode(ino, &inode)) continue;
        if(inode.i_mode == 0) continue;
        // direct
        for(int d = 0; d < 12; d++) mark_block(inode.i_block[d]);
        U32 EntriesPerBlock = m_BlockSize / sizeof(U32);
        // single indirect
        if(inode.i_block[12]){
            PageAlloc::DMAAlloc::DMABuffer* ibuf = nullptr;
            if(ReadBlock(inode.i_block[12], &ibuf)){
                U32* ptrs = (U32*)ibuf->VirtAddr;
                for(U32 j=0;j<EntriesPerBlock;j++) mark_block(ptrs[j]);
                PageAlloc::DMAAlloc::FreeDMABuffer(ibuf);
            }
        }
        // double indirect
        if(inode.i_block[13]){
            PageAlloc::DMAAlloc::DMABuffer* dbuf = nullptr;
            if(ReadBlock(inode.i_block[13], &dbuf)){
                U32* lvl1 = (U32*)dbuf->VirtAddr;
                for(U32 i1=0;i1<EntriesPerBlock;i1++){
                    if(lvl1[i1] == 0) continue;
                    PageAlloc::DMAAlloc::DMABuffer* sbuf = nullptr;
                    if(ReadBlock(lvl1[i1], &sbuf)){
                        U32* ptrs = (U32*)sbuf->VirtAddr;
                        for(U32 j=0;j<EntriesPerBlock;j++) mark_block(ptrs[j]);
                        PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);
                    }
                }
                PageAlloc::DMAAlloc::FreeDMABuffer(dbuf);
            }
        }
        // triple indirect
        if(inode.i_block[14]){
            PageAlloc::DMAAlloc::DMABuffer* tbuf = nullptr;
            if(ReadBlock(inode.i_block[14], &tbuf)){
                U32* lvl1 = (U32*)tbuf->VirtAddr;
                for(U32 i1=0;i1<EntriesPerBlock;i1++){
                    if(lvl1[i1] == 0) continue;
                    PageAlloc::DMAAlloc::DMABuffer* mbuf = nullptr;
                    if(ReadBlock(lvl1[i1], &mbuf)){
                        U32* lvl2 = (U32*)mbuf->VirtAddr;
                        for(U32 i2=0;i2<EntriesPerBlock;i2++){
                            if(lvl2[i2] == 0) continue;
                            PageAlloc::DMAAlloc::DMABuffer* sbuf = nullptr;
                            if(ReadBlock(lvl2[i2], &sbuf)){
                                U32* ptrs = (U32*)sbuf->VirtAddr;
                                for(U32 j=0;j<EntriesPerBlock;j++) mark_block(ptrs[j]);
                                PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);
                            }
                        }
                        PageAlloc::DMAAlloc::FreeDMABuffer(mbuf);
                    }
                }
                PageAlloc::DMAAlloc::FreeDMABuffer(tbuf);
            }
        }
    }

    // Now compare to block bitmaps
    U32 inconsistencies = 0;
    U32 totalBitmapSet = 0;
    for(U32 g = 0; g < m_NumBlockGroups; g++){
        U32 bmpBlock = m_BGDT[g].bg_block_bitmap;
        PageAlloc::DMAAlloc::DMABuffer* bbuf = nullptr;
        if(!ReadBlock(bmpBlock, &bbuf)){ Printk::Write(Printk::Level::LOG_ERR, "EXT2: DebugConsistencyCheck - failed to read block bitmap %u\n", bmpBlock); continue; }
        U8* data = (U8*)bbuf->VirtAddr;
        U32 blocksThisGroup = m_Superblock.s_blocks_per_group;
        for(U32 i = 0; i < blocksThisGroup; i++){
            U32 global = g * m_Superblock.s_blocks_per_group + i + 1;
            if(global > totalBlocks) break;
            U32 byteIdx = i / 8;
            U8 bit = i % 8;
            BOOL bitmapSet = (data[byteIdx] & (1 << bit)) ? TRUE : FALSE;
            U32 idx = global - 1;
            BOOL referenced = (refBitmap[idx/8] & (1 << (idx%8))) ? TRUE : FALSE;
            if(bitmapSet) totalBitmapSet++;
            if(referenced && !bitmapSet){
                Printk::Write(Printk::Level::LOG_ERR, "EXT2: DebugConsistencyCheck - data block %u referenced by inode(s) but bitmap cleared\n", global);
                inconsistencies++;
            }
            if(bitmapSet && !referenced){
                // block marked used but not referenced (possible leak)
                Printk::Write(Printk::Level::LOG_WARNING, "EXT2: DebugConsistencyCheck - data block %u marked used in bitmap but not referenced\n", global);
                inconsistencies++;
            }
        }
        PageAlloc::DMAAlloc::FreeDMABuffer(bbuf);
    }

    // Inode bitmap checks: count used inodes from bitmap and actual used inodes
    U32 totalBitmapInodesSet = 0;
    U32 actualUsedInodes = 0;
    for(U32 g = 0; g < m_NumBlockGroups; g++){
        U32 ibmp = m_BGDT[g].bg_inode_bitmap;
        PageAlloc::DMAAlloc::DMABuffer* ibuf = nullptr;
        if(!ReadBlock(ibmp, &ibuf)){ Printk::Write(Printk::Level::LOG_ERR, "EXT2: DebugConsistencyCheck - failed to read inode bitmap %u\n", ibmp); continue; }
        U8* data = (U8*)ibuf->VirtAddr;
        U32 inodesThisGroup = m_Superblock.s_inodes_per_group;
        for(U32 i=0;i<inodesThisGroup;i++){
            U32 global = g * m_Superblock.s_inodes_per_group + i + 1;
            if(global > totalInodes) break;
            U32 byteIdx = i / 8;
            U8 bit = i % 8;
            BOOL bitmapSet = (data[byteIdx] & (1 << bit)) ? TRUE : FALSE;
            if(bitmapSet) totalBitmapInodesSet++;
            EXT2::Inode inode;
            if(!ReadInode(global, &inode)) continue;
            if(inode.i_mode != 0) actualUsedInodes++;
            if(bitmapSet && inode.i_mode == 0){
                Printk::Write(Printk::Level::LOG_ERR, "EXT2: DebugConsistencyCheck - inode %u marked used in bitmap but inode table empty\n", global);
                inconsistencies++;
            }
            if(!bitmapSet && inode.i_mode != 0){
                Printk::Write(Printk::Level::LOG_ERR, "EXT2: DebugConsistencyCheck - inode %u not marked in bitmap but inode table in-use\n", global);
                inconsistencies++;
            }
        }
        PageAlloc::DMAAlloc::FreeDMABuffer(ibuf);
    }

    Printk::Write(Printk::Level::LOG_DEBUG, "EXT2: DebugConsistencyCheck - total bitmap blocks set: %u, total inode bitmap set: %u, actual used inodes: %u, inconsistencies: %u\n", totalBitmapSet, totalBitmapInodesSet, actualUsedInodes, inconsistencies);

    Kmalloc::Free(refBitmap);

    return (inconsistencies == 0) ? TRUE : FALSE;
}

// Conservative repair: set bitmap bits for referenced-but-cleared, and clear bitmap bits for set-but-unreferenced.
// For inode mismatches, call FreeInode for inode-bitset-but-empty, and set bitmap for inode-present-but-cleared.
U32 EXT2FileSystem::DebugRepairConsistency(){
    Printk::Write(Printk::Level::LOG_DEBUG, "EXT2: DebugRepairConsistency - begin\n");
    if(!m_Partition){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: DebugRepairConsistency - not mounted\n");
        return 0;
    }

    U32 totalBlocks = m_Superblock.s_blocks_count;
    U32 totalInodes = m_Superblock.s_inodes_count;
    U32 EntriesPerBlock = m_BlockSize / sizeof(U32);

    U32 bmBytes = (totalBlocks + 7) / 8;
    U8* refBitmap = (U8*)Kmalloc::Alloc(bmBytes);
    if(!refBitmap){ Printk::Write(Printk::Level::LOG_ERR, "EXT2: DebugRepairConsistency - alloc failed\n"); return 0; }
    String::Memset(refBitmap, 0, bmBytes);

    auto mark_block = [&](U32 b){ if(b == 0 || b > totalBlocks) return; U32 idx = b - 1; refBitmap[idx/8] |= (1 << (idx%8)); };

    // Walk inodes and mark referenced data blocks
    for(U32 ino = 1; ino <= totalInodes; ino++){
        EXT2::Inode inode;
        if(!ReadInode(ino, &inode)) continue;
        if(inode.i_mode == 0) continue;
        // direct
        for(int d=0; d<12; d++) mark_block(inode.i_block[d]);
        if(inode.i_block[12]){
            PageAlloc::DMAAlloc::DMABuffer* ibuf = nullptr;
            if(ReadBlock(inode.i_block[12], &ibuf)){
                U32* ptrs = (U32*)ibuf->VirtAddr;
                for(U32 j=0;j<EntriesPerBlock;j++) mark_block(ptrs[j]);
                PageAlloc::DMAAlloc::FreeDMABuffer(ibuf);
            }
        }
        if(inode.i_block[13]){
            PageAlloc::DMAAlloc::DMABuffer* dbuf = nullptr;
            if(ReadBlock(inode.i_block[13], &dbuf)){
                U32* lvl1 = (U32*)dbuf->VirtAddr;
                for(U32 i1=0;i1<EntriesPerBlock;i1++){
                    if(lvl1[i1] == 0) continue;
                    PageAlloc::DMAAlloc::DMABuffer* sbuf = nullptr;
                    if(ReadBlock(lvl1[i1], &sbuf)){
                        U32* ptrs = (U32*)sbuf->VirtAddr;
                        for(U32 j=0;j<EntriesPerBlock;j++) mark_block(ptrs[j]);
                        PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);
                    }
                }
                PageAlloc::DMAAlloc::FreeDMABuffer(dbuf);
            }
        }
        if(inode.i_block[14]){
            PageAlloc::DMAAlloc::DMABuffer* tbuf = nullptr;
            if(ReadBlock(inode.i_block[14], &tbuf)){
                U32* lvl1 = (U32*)tbuf->VirtAddr;
                for(U32 i1=0;i1<EntriesPerBlock;i1++){
                    if(lvl1[i1] == 0) continue;
                    PageAlloc::DMAAlloc::DMABuffer* mbuf = nullptr;
                    if(ReadBlock(lvl1[i1], &mbuf)){
                        U32* lvl2 = (U32*)mbuf->VirtAddr;
                        for(U32 i2=0;i2<EntriesPerBlock;i2++){
                            if(lvl2[i2] == 0) continue;
                            PageAlloc::DMAAlloc::DMABuffer* sbuf = nullptr;
                            if(ReadBlock(lvl2[i2], &sbuf)){
                                U32* ptrs = (U32*)sbuf->VirtAddr;
                                for(U32 j=0;j<EntriesPerBlock;j++) mark_block(ptrs[j]);
                                PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);
                            }
                        }
                        PageAlloc::DMAAlloc::FreeDMABuffer(mbuf);
                    }
                }
                PageAlloc::DMAAlloc::FreeDMABuffer(tbuf);
            }
        }
    }

    U32 fixes = 0;

    // Repair block bitmaps group-by-group
    for(U32 g = 0; g < m_NumBlockGroups; g++){
        U32 bmpBlock = m_BGDT[g].bg_block_bitmap;
        PageAlloc::DMAAlloc::DMABuffer* bbuf = nullptr;
        if(!ReadBlock(bmpBlock, &bbuf)){ Printk::Write(Printk::Level::LOG_ERR, "EXT2: Repair - failed to read block bitmap %u\n", bmpBlock); continue; }
        U8* data = (U8*)bbuf->VirtAddr;
        U32 blocksThisGroup = m_Superblock.s_blocks_per_group;
        BOOL changed = FALSE;
        for(U32 i = 0; i < blocksThisGroup; i++){
            U32 global = g * m_Superblock.s_blocks_per_group + i + 1;
            if(global > totalBlocks) break;
            U32 byteIdx = i / 8;
            U8 bit = i % 8;
            BOOL bitmapSet = (data[byteIdx] & (1 << bit)) ? TRUE : FALSE;
            U32 idx = global - 1;
            BOOL referenced = (refBitmap[idx/8] & (1 << (idx%8))) ? TRUE : FALSE;
            if(referenced && !bitmapSet){
                // mark used
                data[byteIdx] |= (1 << bit);
                if(m_BGDT[g].bg_free_blocks_count > 0) m_BGDT[g].bg_free_blocks_count--;
                if(m_Superblock.s_free_blocks_count > 0) m_Superblock.s_free_blocks_count--;
                fixes++;
                changed = TRUE;
            }
            if(bitmapSet && !referenced){
                // free leaked block
                data[byteIdx] &= (U8)(~(1 << bit));
                m_BGDT[g].bg_free_blocks_count++;
                if(m_Superblock.s_free_blocks_count < 0xFFFFFFFF) m_Superblock.s_free_blocks_count++;
                fixes++;
                changed = TRUE;
            }
        }
        if(changed){
            if(!WriteBlock(bmpBlock, bbuf)){
                Printk::Write(Printk::Level::LOG_ERR, "EXT2: Repair - failed to write block bitmap %u\n", bmpBlock);
            }
        }
        PageAlloc::DMAAlloc::FreeDMABuffer(bbuf);
    }

    // Repair inode bitmaps
    for(U32 g = 0; g < m_NumBlockGroups; g++){
        U32 ibmp = m_BGDT[g].bg_inode_bitmap;
        PageAlloc::DMAAlloc::DMABuffer* ibuf = nullptr;
        if(!ReadBlock(ibmp, &ibuf)){ Printk::Write(Printk::Level::LOG_ERR, "EXT2: Repair - failed to read inode bitmap %u\n", ibmp); continue; }
        U8* data = (U8*)ibuf->VirtAddr;
        U32 inodesThisGroup = m_Superblock.s_inodes_per_group;
        BOOL changed = FALSE;
        for(U32 i=0;i<inodesThisGroup;i++){
            U32 global = g * m_Superblock.s_inodes_per_group + i + 1;
            if(global > totalInodes) break;
            U32 byteIdx = i / 8;
            U8 bit = i % 8;
            BOOL bitmapSet = (data[byteIdx] & (1 << bit)) ? TRUE : FALSE;
            EXT2::Inode inode;
            if(!ReadInode(global, &inode)) continue;
            BOOL inodePresent = (inode.i_mode != 0) ? TRUE : FALSE;
            if(inodePresent && !bitmapSet){
                // mark inode used
                data[byteIdx] |= (1 << bit);
                if(m_BGDT[g].bg_free_inodes_count > 0) m_BGDT[g].bg_free_inodes_count--;
                if(m_Superblock.s_free_inodes_count > 0) m_Superblock.s_free_inodes_count--;
                fixes++;
                changed = TRUE;
            }
            if(bitmapSet && !inodePresent){
                // free inode (clear bitmap and zero inode)
                FreeInode(global);
                fixes++;
                changed = TRUE;
            }
        }
        if(changed){
            if(!WriteBlock(ibmp, ibuf)){
                Printk::Write(Printk::Level::LOG_ERR, "EXT2: Repair - failed to write inode bitmap %u\n", ibmp);
            }
        }
        PageAlloc::DMAAlloc::FreeDMABuffer(ibuf);
    }

    // Persist superblock and BGDT after repairs
    PersistSuperblockAndBGDT();

    Kmalloc::Free(refBitmap);
    Printk::Write(Printk::Level::LOG_DEBUG, "EXT2: DebugRepairConsistency - done; fixes=%u\n", fixes);
    return fixes;
}

// Debug-only: recursively remove a path (file or directory) ignoring permissions and
// directory-empty checks. This is a best-effort recovery tool and should only be used
// from test/repair paths.
BOOL EXT2FileSystem::DebugForceRemove(const char* path){
    if(!path || String::Strlen(path) == 0) return FALSE;

    // Find target inode
    EXT2::Inode targetInode;
    U32 ino = FindInodeForPath(path, &targetInode);
    if(ino == 0){ Printk::Write(Printk::Level::LOG_WARNING, "EXT2: DebugForceRemove - path not found: %s\n", path); return FALSE; }

    // If regular file: remove directory entry from parent and free inode
    if(!(targetInode.i_mode & EXT2_S_IFDIR)){
        // Parse parent/name
        CHAR8 Parent[256]; CHAR8 Name[256];
        ParsePath((const CHAR8*)path, Parent, Name);
        EXT2::Inode parentInode;
        U32 parentNum = FindInodeForPath((const char*)Parent, &parentInode);
        if(parentNum == 0){ Printk::Write(Printk::Level::LOG_ERR, "EXT2: DebugForceRemove - parent not found for %s\n", path); return FALSE; }
        // Try to remove entry if present (best-effort)
        RemoveEntryFromDirectory(&parentInode, Name);
        // Free inode (and blocks)
        // mimic Delete's freeing path: clear blocks and FreeInode
        for(int i=0;i<12;i++){ if(targetInode.i_block[i]){ FreeBlock(targetInode.i_block[i]); targetInode.i_block[i]=0; } }
        U32 EntriesPerBlock = m_BlockSize / sizeof(U32);
        if(targetInode.i_block[12]){ PageAlloc::DMAAlloc::DMABuffer* ibuf=nullptr; if(ReadBlock(targetInode.i_block[12], &ibuf)){ U32* ptrs=(U32*)ibuf->VirtAddr; for(U32 j=0;j<EntriesPerBlock;j++) if(ptrs[j]){ FreeBlock(ptrs[j]); ptrs[j]=0; } PageAlloc::DMAAlloc::FreeDMABuffer(ibuf);} FreeBlock(targetInode.i_block[12]); targetInode.i_block[12]=0; }
        if(targetInode.i_block[13]){ PageAlloc::DMAAlloc::DMABuffer* dbuf=nullptr; if(ReadBlock(targetInode.i_block[13], &dbuf)){ U32* lvl1=(U32*)dbuf->VirtAddr; for(U32 i1=0;i1<EntriesPerBlock;i1++){ U32 first = lvl1[i1]; if(!first) continue; PageAlloc::DMAAlloc::DMABuffer* sbuf=nullptr; if(ReadBlock(first, &sbuf)){ U32* lvl2=(U32*)sbuf->VirtAddr; for(U32 j=0;j<EntriesPerBlock;j++) if(lvl2[j]){ FreeBlock(lvl2[j]); lvl2[j]=0; } PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);} FreeBlock(first);} PageAlloc::DMAAlloc::FreeDMABuffer(dbuf);} FreeBlock(targetInode.i_block[13]); targetInode.i_block[13]=0; }
        if(targetInode.i_block[14]){ PageAlloc::DMAAlloc::DMABuffer* tbuf=nullptr; if(ReadBlock(targetInode.i_block[14], &tbuf)){ U32* lvl1=(U32*)tbuf->VirtAddr; for(U32 i1=0;i1<EntriesPerBlock;i1++){ U32 lvl2_block = lvl1[i1]; if(!lvl2_block) continue; PageAlloc::DMAAlloc::DMABuffer* mbuf=nullptr; if(ReadBlock(lvl2_block, &mbuf)){ U32* lvl2=(U32*)mbuf->VirtAddr; for(U32 i2=0;i2<EntriesPerBlock;i2++){ if(lvl2[i2]==0) continue; PageAlloc::DMAAlloc::DMABuffer* sbuf=nullptr; if(ReadBlock(lvl2[i2], &sbuf)){ U32* ptrs=(U32*)sbuf->VirtAddr; for(U32 j=0;j<EntriesPerBlock;j++) if(ptrs[j]){ FreeBlock(ptrs[j]); ptrs[j]=0; } PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);} FreeBlock(lvl2[i2]); } PageAlloc::DMAAlloc::FreeDMABuffer(mbuf);} FreeBlock(lvl2_block);} PageAlloc::DMAAlloc::FreeDMABuffer(tbuf);} FreeBlock(targetInode.i_block[14]); targetInode.i_block[14]=0; }
        // Zero size and blocks
        targetInode.i_size = 0; targetInode.i_blocks = 0;
        FreeInode(ino);
        PersistSuperblockAndBGDT();
        WriteInode(parentNum, &parentInode); // best-effort persist parent
        return TRUE;
    }

    // Directory: recursively iterate entries and remove them, then remove directory itself.
    // We'll build child paths on the fly.
    // Parse parent and base name
    CHAR8 ParentPath[256]; CHAR8 BaseName[256];
    ParsePath((const CHAR8*)path, ParentPath, BaseName);

    // Iterate directory data blocks to collect names (we'll store up to reasonable limit per block)
    U32 EntriesPerBlock = m_BlockSize / sizeof(U32);
    // Helper to iterate entries in a block
    auto iterate_block = [&](U32 blockNum){
        if(blockNum == 0) return;
        PageAlloc::DMAAlloc::DMABuffer* dbuf = nullptr;
        if(!ReadBlock(blockNum, &dbuf)) return;
        U8* p = (U8*)dbuf->VirtAddr;
        U32 off = 0;
        while(off < m_BlockSize){
            struct DirentHeader { U32 inode; U16 rec_len; U8 name_len; U8 file_type; } hdr;
            String::Memcpy(&hdr, p + off, sizeof(hdr));
            if(hdr.rec_len == 0) break;
            if(hdr.inode != 0){
                CHAR8 namebuf[256]; U32 nl = hdr.name_len; if(nl > 255) nl = 255;
                String::Memcpy(namebuf, p + off + sizeof(hdr), nl); namebuf[nl] = '\0';
                if(!ext2_dirent_name_compare(namebuf, hdr.name_len, ".") && !ext2_dirent_name_compare(namebuf, hdr.name_len, "..")){
                    // Build child path
                    CHAR8 child[512]; String::Strcpy(child, path);
                    if(child[String::Strlen(child)-1] != '/') String::Strcat(child, "/");
                    String::Strcat(child, namebuf);
                    // Recurse
                    DebugForceRemove((const char*)child);
                }
            }
            off += hdr.rec_len;
            if(hdr.rec_len == 0) break;
        }
        PageAlloc::DMAAlloc::FreeDMABuffer(dbuf);
    };

    // direct
    for(int i=0;i<12;i++) if(targetInode.i_block[i]) iterate_block(targetInode.i_block[i]);
    // single
    if(targetInode.i_block[12]){
        PageAlloc::DMAAlloc::DMABuffer* ibuf = nullptr;
        if(ReadBlock(targetInode.i_block[12], &ibuf)){
            U32* ptrs = (U32*)ibuf->VirtAddr;
            for(U32 j=0;j<EntriesPerBlock;j++) if(ptrs[j]) iterate_block(ptrs[j]);
            PageAlloc::DMAAlloc::FreeDMABuffer(ibuf);
        }
    }
    // double
    if(targetInode.i_block[13]){
        PageAlloc::DMAAlloc::DMABuffer* dbuf = nullptr;
        if(ReadBlock(targetInode.i_block[13], &dbuf)){
            U32* lvl1 = (U32*)dbuf->VirtAddr;
            for(U32 i1=0;i1<EntriesPerBlock;i1++){
                if(lvl1[i1]==0) continue;
                PageAlloc::DMAAlloc::DMABuffer* sbuf = nullptr;
                if(ReadBlock(lvl1[i1], &sbuf)){
                    U32* ptrs = (U32*)sbuf->VirtAddr;
                    for(U32 j=0;j<EntriesPerBlock;j++) if(ptrs[j]) iterate_block(ptrs[j]);
                    PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);
                }
            }
            PageAlloc::DMAAlloc::FreeDMABuffer(dbuf);
        }
    }
    // triple
    if(targetInode.i_block[14]){
        PageAlloc::DMAAlloc::DMABuffer* tbuf = nullptr;
        if(ReadBlock(targetInode.i_block[14], &tbuf)){
            U32* lvl1 = (U32*)tbuf->VirtAddr;
            for(U32 i1=0;i1<EntriesPerBlock;i1++){
                if(lvl1[i1]==0) continue;
                PageAlloc::DMAAlloc::DMABuffer* mbuf = nullptr;
                if(ReadBlock(lvl1[i1], &mbuf)){
                    U32* lvl2 = (U32*)mbuf->VirtAddr;
                    for(U32 i2=0;i2<EntriesPerBlock;i2++){
                        if(lvl2[i2]==0) continue;
                        PageAlloc::DMAAlloc::DMABuffer* sbuf = nullptr;
                        if(ReadBlock(lvl2[i2], &sbuf)){
                            U32* ptrs = (U32*)sbuf->VirtAddr;
                            for(U32 j=0;j<EntriesPerBlock;j++) if(ptrs[j]) iterate_block(ptrs[j]);
                            PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);
                        }
                    }
                    PageAlloc::DMAAlloc::FreeDMABuffer(mbuf);
                }
            }
            PageAlloc::DMAAlloc::FreeDMABuffer(tbuf);
        }
    }

    // After children removed, remove directory entry from parent and free its blocks and inode
    // Remove entry from parent
    CHAR8 parentP[256]; CHAR8 baseN[256];
    ParsePath((const CHAR8*)path, parentP, baseN);
    EXT2::Inode parentInode;
    U32 parentNum = FindInodeForPath((const char*)parentP, &parentInode);
    if(parentNum != 0) RemoveEntryFromDirectory(&parentInode, baseN);

    // free blocks (direct + indirect) similar to file case
    for(int i=0;i<12;i++){ if(targetInode.i_block[i]){ FreeBlock(targetInode.i_block[i]); targetInode.i_block[i]=0; } }
    if(targetInode.i_block[12]){ PageAlloc::DMAAlloc::DMABuffer* ibuf=nullptr; if(ReadBlock(targetInode.i_block[12], &ibuf)){ U32* ptrs=(U32*)ibuf->VirtAddr; for(U32 j=0;j<EntriesPerBlock;j++) if(ptrs[j]){ FreeBlock(ptrs[j]); ptrs[j]=0; } PageAlloc::DMAAlloc::FreeDMABuffer(ibuf);} FreeBlock(targetInode.i_block[12]); targetInode.i_block[12]=0; }
    if(targetInode.i_block[13]){ PageAlloc::DMAAlloc::DMABuffer* dbuf=nullptr; if(ReadBlock(targetInode.i_block[13], &dbuf)){ U32* lvl1=(U32*)dbuf->VirtAddr; for(U32 i1=0;i1<EntriesPerBlock;i1++){ U32 first = lvl1[i1]; if(!first) continue; PageAlloc::DMAAlloc::DMABuffer* sbuf=nullptr; if(ReadBlock(first, &sbuf)){ U32* lvl2=(U32*)sbuf->VirtAddr; for(U32 j=0;j<EntriesPerBlock;j++) if(lvl2[j]){ FreeBlock(lvl2[j]); lvl2[j]=0; } PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);} FreeBlock(first);} PageAlloc::DMAAlloc::FreeDMABuffer(dbuf);} FreeBlock(targetInode.i_block[13]); targetInode.i_block[13]=0; }
    if(targetInode.i_block[14]){ PageAlloc::DMAAlloc::DMABuffer* tbuf=nullptr; if(ReadBlock(targetInode.i_block[14], &tbuf)){ U32* lvl1=(U32*)tbuf->VirtAddr; for(U32 i1=0;i1<EntriesPerBlock;i1++){ U32 lvl2_block = lvl1[i1]; if(!lvl2_block) continue; PageAlloc::DMAAlloc::DMABuffer* mbuf=nullptr; if(ReadBlock(lvl2_block, &mbuf)){ U32* lvl2=(U32*)mbuf->VirtAddr; for(U32 i2=0;i2<EntriesPerBlock;i2++){ if(lvl2[i2]==0) continue; PageAlloc::DMAAlloc::DMABuffer* sbuf=nullptr; if(ReadBlock(lvl2[i2], &sbuf)){ U32* ptrs=(U32*)sbuf->VirtAddr; for(U32 j=0;j<EntriesPerBlock;j++) if(ptrs[j]){ FreeBlock(ptrs[j]); ptrs[j]=0; } PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);} FreeBlock(lvl2[i2]); } PageAlloc::DMAAlloc::FreeDMABuffer(mbuf);} FreeBlock(lvl2_block);} PageAlloc::DMAAlloc::FreeDMABuffer(tbuf);} FreeBlock(targetInode.i_block[14]); targetInode.i_block[14]=0; }
    targetInode.i_size = 0; targetInode.i_blocks = 0;
    FreeInode(ino);
    PersistSuperblockAndBGDT();
    if(parentNum != 0) WriteInode(parentNum, &parentInode);
    return TRUE;
}

U32 EXT2FileSystem::AllocateInode(){
    PageAlloc::DMAAlloc::DMABuffer *BlockBuffer = nullptr;
    
    for(U32 Group = 0; Group < m_NumBlockGroups; Group++){
        if(m_BGDT[Group].bg_free_inodes_count > 0){
            U32 InodeBitmapBlock = m_BGDT[Group].bg_inode_bitmap;

            if(!ReadBlock(InodeBitmapBlock, &BlockBuffer)){
                Printk::Write(Printk::Level::LOG_ERR, "EXT2: AllocateInode - Failed to read inode bitmap block %u\n", InodeBitmapBlock);
                return 0;
            }

            U8 *BitmapData = (U8*)BlockBuffer->VirtAddr;
            VAL32 FreeBitIndex = -1;
            U32 InodesThisGroup = m_Superblock.s_inodes_per_group;
            U32 BitmapBytes = (InodesThisGroup + 7) / 8;

            for(U32 ByteIdx = 0; ByteIdx < BitmapBytes; ByteIdx++){
                if(BitmapData[ByteIdx] == 0xFF) continue;

                for(U8 Bit = 0; Bit < 8; Bit++){
                    if(!(BitmapData[ByteIdx] & (1 << Bit))){
                        VAL32 Bites = (ByteIdx * 8) + Bit;

                        if((U32)Bites >= InodesThisGroup){
                            FreeBitIndex = -1;
                            break;
                        }

                        FreeBitIndex = Bites;

                        BitmapData[ByteIdx] |= (1 << Bit);
                        break;
                    }
                }

                if (FreeBitIndex != -1) {
                    break; // Stop cari byte
                }
            }

            if (FreeBitIndex == -1) {
                // Seharusnya tidak terjadi jika bg_free_inodes_count > 0
                Printk::Write(Printk::Level::LOG_WARNING, "EXT2: Group %u shows free inodes, but bitmap scan failed.\n", Group);
                PageAlloc::DMAAlloc::FreeDMABuffer(BlockBuffer);
                continue; // Coba grup berikutnya
            }

            // Try writing the updated inode bitmap with a small retry loop to
            // tolerate transient write failures (useful during stress/failure
            // injection testing). If all attempts fail, return failure.
            BOOL write_ok = FALSE;
            for(int attempt = 0; attempt < 3; ++attempt){
                if(WriteBlock(InodeBitmapBlock, BlockBuffer)){
                    write_ok = TRUE;
                    break;
                }
                Printk::Write(Printk::Level::LOG_WARNING, "EXT2: AllocateInode - Write attempt %d failed for inode bitmap block %u\n", attempt+1, InodeBitmapBlock);
            }

            if(!write_ok){
                Printk::Write(Printk::Level::LOG_ERR, "EXT2: AllocateInode - Failed to write updated inode bitmap block %u after retries\n", InodeBitmapBlock);
                PageAlloc::DMAAlloc::FreeDMABuffer(BlockBuffer);
                return 0;
            }

            PageAlloc::DMAAlloc::FreeDMABuffer(BlockBuffer);

            m_BGDT[Group].bg_free_inodes_count--;
            m_Superblock.s_free_inodes_count--;
            // TODO: Seharusnya update BGDT sama Superblock juga

            U32 InodeNum = (Group * m_Superblock.s_inodes_per_group) + (FreeBitIndex + 1);
            Printk::Write(Printk::Level::LOG_DEBUG, "EXT2: Allocated inode %u from group %u\n", InodeNum, Group);
            return InodeNum;
        }
    }

    Printk::Write(Printk::Level::LOG_ERR, "EXT2: AllocateInode - No free inodes available.\n"); 
    return 0;
}

U32 EXT2FileSystem::AllocateBlock(){
    Printk::Write(Printk::Level::LOG_WARNING, "EXT2: AllocateBlock()\n");

    PageAlloc::DMAAlloc::DMABuffer *BlockBuffer = nullptr;

    for(U32 Group = 0; Group < m_NumBlockGroups; Group++){
        if(m_BGDT[Group].bg_free_blocks_count > 0){
            Printk::Write(Printk::Level::LOG_DEBUG, "EXT2: AllocateBlock - Found free blocks in group %u\n", Group);     

            U32 BlockBitmapBlock = m_BGDT[Group].bg_block_bitmap;

            if(!ReadBlock(BlockBitmapBlock, &BlockBuffer)){
                Printk::Write(Printk::Level::LOG_ERR, "EXT2: AllocateBlock - Failed to read block bitmap block %u\n", BlockBitmapBlock);
                continue;
            }

            U8 *BitmapData = (U8*)BlockBuffer->VirtAddr;
            VAL32 FreeBitIndex = -1;
            U32 BlockThisGroup = m_Superblock.s_blocks_per_group;
            U32 BitmapBytes = (BlockThisGroup + 7) / 8;

            for(U32 ByteIdx = 0; ByteIdx < BitmapBytes; ByteIdx++){
                if(BitmapData[ByteIdx] == 0xFF){
                    continue;
                }

                for(U8 Bit = 0; Bit < 8; Bit++){
                    if(!(BitmapData[ByteIdx] & (1 << Bit))){
                        VAL32 Bites = (ByteIdx * 8) + Bit;

                        if((U32)Bites >= BlockThisGroup){
                            FreeBitIndex = -1;
                            break;
                        }

                        FreeBitIndex = Bites;

                        BitmapData[ByteIdx] |= (1 << Bit);
                        break;
                    }
                }

                if(FreeBitIndex != -1){
                    break; // Stop cari byte
                }
            }

            if(FreeBitIndex == -1){
                Printk::Write(Printk::Level::LOG_WARNING, "EXT2: AllocateBlock - Group %u shows free blocks, but bitmap scan failed.\n", Group);
                PageAlloc::DMAAlloc::FreeDMABuffer(BlockBuffer);
                continue; // Coba grup berikutnya
            }

            if(!WriteBlock(BlockBitmapBlock, BlockBuffer)){
                Printk::Write(Printk::Level::LOG_ERR, "EXT2: AllocateBlock - Failed to write updated block bitmap block %u\n", BlockBitmapBlock);
                PageAlloc::DMAAlloc::FreeDMABuffer(BlockBuffer);   
                return 0;
            }

            PageAlloc::DMAAlloc::FreeDMABuffer(BlockBuffer);

            m_BGDT[Group].bg_free_blocks_count--;
            m_Superblock.s_free_blocks_count--;

            U32 BlockNum = (Group * m_Superblock.s_blocks_per_group) + (FreeBitIndex + 1);

            Printk::Write(Printk::Level::LOG_DEBUG, "EXT2: Allocated block %u from group %u\n", BlockNum, Group);

            return BlockNum;
        }
    }

    Printk::Write(Printk::Level::LOG_ERR, "EXT2: AllocateBlock - No free blocks available.\n");
    return 0;
}

// Free a single block: clear bitmap and update BGDT/superblock counts
BOOL EXT2FileSystem::FreeBlock(U32 BlockNum){
    if(BlockNum == 0) return FALSE;

    U32 Group = (BlockNum - 1) / m_Superblock.s_blocks_per_group;
    if(Group >= m_NumBlockGroups) return FALSE;

    U32 bitIndex = (BlockNum - 1) % m_Superblock.s_blocks_per_group;
    U32 bitmapBlock = m_BGDT[Group].bg_block_bitmap;

    PageAlloc::DMAAlloc::DMABuffer* buf = nullptr;
    if(!ReadBlock(bitmapBlock, &buf)){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: FreeBlock - failed to read bitmap block %u\n", bitmapBlock);
        return FALSE;
    }

    U8* data = (U8*)buf->VirtAddr;
    U32 byteIdx = bitIndex / 8;
    U8 bit = bitIndex % 8;

    data[byteIdx] &= (U8)(~(1 << bit));

    if(!WriteBlock(bitmapBlock, buf)){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: FreeBlock - failed to write bitmap block %u\n", bitmapBlock);
        PageAlloc::DMAAlloc::FreeDMABuffer(buf);
        return FALSE;
    }

    PageAlloc::DMAAlloc::FreeDMABuffer(buf);

    // Update counters
    m_BGDT[Group].bg_free_blocks_count++;
    if(m_Superblock.s_free_blocks_count < 0xFFFFFFFF) m_Superblock.s_free_blocks_count++;

    return TRUE;
}

// Free an inode: clear its bitmap bit, zero inode table entry and update counts
BOOL EXT2FileSystem::FreeInode(U32 InodeNum){
    if(InodeNum == 0) return FALSE;

    U32 Group = (InodeNum - 1) / m_Superblock.s_inodes_per_group;
    if(Group >= m_NumBlockGroups) return FALSE;

    U32 bitIndex = (InodeNum - 1) % m_Superblock.s_inodes_per_group;
    U32 bitmapBlock = m_BGDT[Group].bg_inode_bitmap;

    PageAlloc::DMAAlloc::DMABuffer* buf = nullptr;
    if(!ReadBlock(bitmapBlock, &buf)){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: FreeInode - failed to read inode bitmap block %u\n", bitmapBlock);
        return FALSE;
    }

    U8* data = (U8*)buf->VirtAddr;
    U32 byteIdx = bitIndex / 8;
    U8 bit = bitIndex % 8;

    data[byteIdx] &= (U8)(~(1 << bit));

    if(!WriteBlock(bitmapBlock, buf)){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: FreeInode - failed to write inode bitmap block %u\n", bitmapBlock);
        PageAlloc::DMAAlloc::FreeDMABuffer(buf);
        return FALSE;
    }

    PageAlloc::DMAAlloc::FreeDMABuffer(buf);

    // Update counts
    m_BGDT[Group].bg_free_inodes_count++;
    if(m_Superblock.s_free_inodes_count < 0xFFFFFFFF) m_Superblock.s_free_inodes_count++;

    // Zero the inode entry in the inode table
    U32 InodeTableBlock = m_BGDT[Group].bg_inode_table;
    U16 InodeSize = m_Superblock.s_inode_size;
    U32 Index = (InodeNum - 1) % m_Superblock.s_inodes_per_group;
    U64 OffsetInTable = (U64)Index * InodeSize;
    U32 BlockOffset = OffsetInTable / m_BlockSize;
    U32 OffsetInBlock = OffsetInTable % m_BlockSize;
    U32 TargetBlock = InodeTableBlock + BlockOffset;

    PageAlloc::DMAAlloc::DMABuffer *ibuf = nullptr;
    if(!ReadBlock(TargetBlock, &ibuf)){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: FreeInode - failed to read inode table block %u\n", TargetBlock);
        return FALSE;
    }

    // zero out inode bytes
    U8* ptr = (U8*)ibuf->VirtAddr + OffsetInBlock;
    for(U32 i = 0; i < sizeof(EXT2::Inode) && (OffsetInBlock + i) < m_BlockSize; i++) ptr[i] = 0;

    if(!WriteBlock(TargetBlock, ibuf)){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2: FreeInode - failed to write inode table block %u\n", TargetBlock);
        PageAlloc::DMAAlloc::FreeDMABuffer(ibuf);
        return FALSE;
    }

    PageAlloc::DMAAlloc::FreeDMABuffer(ibuf);

    return TRUE;
}

// Persist superblock and BGDT to disk after metadata changes
BOOL EXT2FileSystem::PersistSuperblockAndBGDT(){
    // Write superblock (match how we read it during mount)
    PageAlloc::DMAAlloc::DMABuffer *sbuf = PageAlloc::DMAAlloc::AllocateDMABytes(512);
    if(sbuf){
        String::Memset((void*)sbuf->VirtAddr, 0, 512);
        String::Memcpy((U8*)sbuf->VirtAddr, (U8*)&m_Superblock, sizeof(EXT2::SuperBlock));
        // superblock was read from sector 2 earlier
        if(!m_Partition->WriteSectors(2, 1, sbuf)){
            Printk::Write(Printk::Level::LOG_WARNING, "EXT2: PersistSuperblockAndBGDT - failed to write superblock sector\n");
        }
        PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);
    }

    // Write BGDT blocks
    U32 BGDTStartBlocks = (m_BlockSize == 1024) ? 2 : 1;
    for(U32 i = 0; i < m_BGDT_SizeInBlocks; i++){
        PageAlloc::DMAAlloc::DMABuffer *tmp = PageAlloc::DMAAlloc::AllocateDMABytes(m_BlockSize);
        if(!tmp){
            Printk::Write(Printk::Level::LOG_WARNING, "EXT2: PersistSuperblockAndBGDT - failed to allocate DMABuffer for BGDT\n");
            return FALSE;
        }
        U8 *src = ((U8*)m_BGDT) + (i * m_BlockSize);
        String::Memcpy((U8*)tmp->VirtAddr, src, m_BlockSize);
        if(!WriteBlock(BGDTStartBlocks + i, tmp)){
            Printk::Write(Printk::Level::LOG_WARNING, "EXT2: PersistSuperblockAndBGDT - failed write BGDT block %u\n", BGDTStartBlocks + i);
        }
        PageAlloc::DMAAlloc::FreeDMABuffer(tmp);
    }

    return TRUE;
}

U32 EXT2FileSystem::GetBlockNumForFileOffset(EXT2::Inode* inode, U32 fileBlockIndex) {
    if (fileBlockIndex < 12) {
        // Direct blocks
        return inode->i_block[fileBlockIndex];
    }

    U32 EntriesPerBlock = m_BlockSize / sizeof(U32); // 4 bytes per block pointer
    U32 idx = fileBlockIndex - 12;

    // single indirect
    if (idx < EntriesPerBlock) {
        U32 IndirectBlockNum = inode->i_block[12];
        if (IndirectBlockNum == 0) return 0;

        PageAlloc::DMAAlloc::DMABuffer* blockBuffer = nullptr;
        if (!ReadBlock(IndirectBlockNum, &blockBuffer)) {
            Printk::Write(Printk::Level::LOG_ERR, "EXT2: Failed to read singly indirect block %u\n", IndirectBlockNum);
            return 0;
        }

        U32 *BlockPointers = (U32*)blockBuffer->VirtAddr;
        U32 TargetBlock = BlockPointers[idx];
        PageAlloc::DMAAlloc::FreeDMABuffer(blockBuffer);
        return TargetBlock;
    }

    idx -= EntriesPerBlock;

    // double indirect
    U64 entries2 = (U64)EntriesPerBlock * (U64)EntriesPerBlock;
    if (idx < entries2) {
        U32 doubleIndirect = inode->i_block[13];
        if (doubleIndirect == 0) return 0;

        PageAlloc::DMAAlloc::DMABuffer* dbuf = nullptr;
        if (!ReadBlock(doubleIndirect, &dbuf)) {
            Printk::Write(Printk::Level::LOG_ERR, "EXT2: Failed to read double indirect block %u\n", doubleIndirect);
            return 0;
        }

        U32 *lvl1 = (U32*)dbuf->VirtAddr;
        U32 idx1 = idx / EntriesPerBlock;
        U32 idx2 = idx % EntriesPerBlock;

        U32 firstLevelBlock = lvl1[idx1];
        PageAlloc::DMAAlloc::FreeDMABuffer(dbuf);
        if (firstLevelBlock == 0) return 0;

        PageAlloc::DMAAlloc::DMABuffer* sbuf = nullptr;
        if(!ReadBlock(firstLevelBlock, &sbuf)){
            Printk::Write(Printk::Level::LOG_ERR, "EXT2: Failed to read double->single indirect block %u\n", firstLevelBlock);
            return 0;
        }

        U32 *lvl2 = (U32*)sbuf->VirtAddr;
        U32 TargetBlock = lvl2[idx2];
        PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);
        return TargetBlock;
    }

    idx -= (U32)entries2;

    // triple indirect
    U64 entries3 = entries2 * (U64)EntriesPerBlock;
    if (idx < entries3) {
        U32 tripleIndirect = inode->i_block[14];
        if (tripleIndirect == 0) return 0;

        PageAlloc::DMAAlloc::DMABuffer* tbuf = nullptr;
        if(!ReadBlock(tripleIndirect, &tbuf)){
            Printk::Write(Printk::Level::LOG_ERR, "EXT2: Failed to read triple indirect block %u\n", tripleIndirect);
            return 0;
        }

        U32 *lvl1 = (U32*)tbuf->VirtAddr;
        U32 idx1 = idx / (U32)entries2;
        U32 rem = idx % (U32)entries2;
        U32 idx2 = rem / EntriesPerBlock;
        U32 idx3 = rem % EntriesPerBlock;

        U32 first = lvl1[idx1];
        PageAlloc::DMAAlloc::FreeDMABuffer(tbuf);
        if (first == 0) return 0;

        PageAlloc::DMAAlloc::DMABuffer* mbuf = nullptr;
        if(!ReadBlock(first, &mbuf)){
            Printk::Write(Printk::Level::LOG_ERR, "EXT2: Failed to read triple->double indirect block %u\n", first);
            return 0;
        }

        U32 *lvl2 = (U32*)mbuf->VirtAddr;
        U32 second = lvl2[idx2];
        PageAlloc::DMAAlloc::FreeDMABuffer(mbuf);
        if (second == 0) return 0;

        PageAlloc::DMAAlloc::DMABuffer* sbuf = nullptr;
        if(!ReadBlock(second, &sbuf)){
            Printk::Write(Printk::Level::LOG_ERR, "EXT2: Failed to read triple->single indirect block %u\n", second);
            return 0;
        }

        U32 *lvl3 = (U32*)sbuf->VirtAddr;
        U32 TargetBlock = lvl3[idx3];
        PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);
        return TargetBlock;
    }

    return 0;
}

// Helper implementation: allocate or get block for fileBlockIndex
U32 EXT2FileSystem::GetOrAllocateBlockForFileOffset(EXT2::Inode* inode, U32 fileBlockIndex){
    // Direct blocks
    if(fileBlockIndex < 12){
        if(inode->i_block[fileBlockIndex] == 0){
            U32 nb = AllocateBlock();
            if(nb == 0) return 0;
            inode->i_block[fileBlockIndex] = nb;
            PageAlloc::DMAAlloc::DMABuffer* z = PageAlloc::DMAAlloc::AllocateDMABytes(m_BlockSize);
            if(z){
                String::Memset((void*)z->VirtAddr, 0, m_BlockSize);
                WriteBlock(nb, z);
                PageAlloc::DMAAlloc::FreeDMABuffer(z);
            }
            inode->i_blocks += (m_BlockSize / 512);
        }
        return inode->i_block[fileBlockIndex];
    }

    U32 EntriesPerBlock = m_BlockSize / sizeof(U32);
    U32 idx = fileBlockIndex - 12;

    // single indirect
    if(idx < EntriesPerBlock){
        if(inode->i_block[12] == 0){
            U32 iblock = AllocateBlock();
            if(iblock == 0) return 0;
            inode->i_block[12] = iblock;
            PageAlloc::DMAAlloc::DMABuffer* b = PageAlloc::DMAAlloc::AllocateDMABytes(m_BlockSize);
            if(!b) return 0;
            String::Memset((void*)b->VirtAddr, 0, m_BlockSize);
            WriteBlock(iblock, b);
            PageAlloc::DMAAlloc::FreeDMABuffer(b);
        }

        PageAlloc::DMAAlloc::DMABuffer* ibuf = nullptr;
        if(!ReadBlock(inode->i_block[12], &ibuf)) return 0;

        U32* pointers = (U32*)ibuf->VirtAddr;
        if(pointers[idx] == 0){
            U32 newblock = AllocateBlock();
            if(newblock == 0){
                PageAlloc::DMAAlloc::FreeDMABuffer(ibuf);
                return 0;
            }
            pointers[idx] = newblock;
            if(!WriteBlock(inode->i_block[12], ibuf)){
                PageAlloc::DMAAlloc::FreeDMABuffer(ibuf);
                return 0;
            }
            PageAlloc::DMAAlloc::DMABuffer* dbuf = PageAlloc::DMAAlloc::AllocateDMABytes(m_BlockSize);
            if(dbuf){
                String::Memset((void*)dbuf->VirtAddr, 0, m_BlockSize);
                WriteBlock(newblock, dbuf);
                PageAlloc::DMAAlloc::FreeDMABuffer(dbuf);
            }
            inode->i_blocks += (m_BlockSize / 512);
        }

        U32 target = pointers[idx];
        PageAlloc::DMAAlloc::FreeDMABuffer(ibuf);
        return target;
    }

    idx -= EntriesPerBlock;

    // double indirect
    U64 entries2 = (U64)EntriesPerBlock * (U64)EntriesPerBlock;
    if(idx < entries2){
        // ensure double indirect block exists
        if(inode->i_block[13] == 0){
            U32 db = AllocateBlock();
            if(db == 0) return 0;
            inode->i_block[13] = db;
            PageAlloc::DMAAlloc::DMABuffer* b = PageAlloc::DMAAlloc::AllocateDMABytes(m_BlockSize);
            if(!b) return 0;
            String::Memset((void*)b->VirtAddr, 0, m_BlockSize);
            WriteBlock(db, b);
            PageAlloc::DMAAlloc::FreeDMABuffer(b);
        }

        PageAlloc::DMAAlloc::DMABuffer* dbuf = nullptr;
        if(!ReadBlock(inode->i_block[13], &dbuf)) return 0;
        U32* lvl1 = (U32*)dbuf->VirtAddr;

        U32 idx1 = idx / EntriesPerBlock;
        U32 idx2 = idx % EntriesPerBlock;

        // ensure first-level (single indirect) block exists
        if(lvl1[idx1] == 0){
            U32 sb = AllocateBlock();
            if(sb == 0){ PageAlloc::DMAAlloc::FreeDMABuffer(dbuf); return 0; }
            lvl1[idx1] = sb;
            if(!WriteBlock(inode->i_block[13], dbuf)){
                PageAlloc::DMAAlloc::FreeDMABuffer(dbuf);
                return 0;
            }
            // zero new single indirect block
            PageAlloc::DMAAlloc::DMABuffer* zb = PageAlloc::DMAAlloc::AllocateDMABytes(m_BlockSize);
            if(zb){ String::Memset((void*)zb->VirtAddr,0,m_BlockSize); WriteBlock(sb, zb); PageAlloc::DMAAlloc::FreeDMABuffer(zb); }
        }

        U32 firstLevel = lvl1[idx1];
        PageAlloc::DMAAlloc::FreeDMABuffer(dbuf);

        // now operate on single indirect block
        PageAlloc::DMAAlloc::DMABuffer* sbuf = nullptr;
        if(!ReadBlock(firstLevel, &sbuf)) return 0;
        U32* pointers = (U32*)sbuf->VirtAddr;
        if(pointers[idx2] == 0){
            U32 newblock = AllocateBlock();
            if(newblock == 0){ PageAlloc::DMAAlloc::FreeDMABuffer(sbuf); return 0; }
            pointers[idx2] = newblock;
            if(!WriteBlock(firstLevel, sbuf)){ PageAlloc::DMAAlloc::FreeDMABuffer(sbuf); return 0; }
            PageAlloc::DMAAlloc::DMABuffer* dataz = PageAlloc::DMAAlloc::AllocateDMABytes(m_BlockSize);
            if(dataz){ String::Memset((void*)dataz->VirtAddr,0,m_BlockSize); WriteBlock(newblock, dataz); PageAlloc::DMAAlloc::FreeDMABuffer(dataz); }
            inode->i_blocks += (m_BlockSize / 512);
        }

        U32 target = pointers[idx2];
        PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);
        return target;
    }

    idx -= (U32)entries2;

    // triple indirect
    U64 entries3 = entries2 * (U64)EntriesPerBlock;
    if(idx < entries3){
        // ensure triple indirect block exists
        if(inode->i_block[14] == 0){
            U32 tb = AllocateBlock();
            if(tb == 0) return 0;
            inode->i_block[14] = tb;
            PageAlloc::DMAAlloc::DMABuffer* b = PageAlloc::DMAAlloc::AllocateDMABytes(m_BlockSize);
            if(!b) return 0;
            String::Memset((void*)b->VirtAddr, 0, m_BlockSize);
            WriteBlock(tb, b);
            PageAlloc::DMAAlloc::FreeDMABuffer(b);
        }

        PageAlloc::DMAAlloc::DMABuffer* tbuf = nullptr;
        if(!ReadBlock(inode->i_block[14], &tbuf)) return 0;
        U32* lvl1 = (U32*)tbuf->VirtAddr;

        U32 idx1 = idx / (U32)entries2;
        U32 rem = idx % (U32)entries2;
        U32 idx2 = rem / EntriesPerBlock;
        U32 idx3 = rem % EntriesPerBlock;

        // ensure lvl2 block exists
        if(lvl1[idx1] == 0){
            U32 db = AllocateBlock();
            if(db == 0){ PageAlloc::DMAAlloc::FreeDMABuffer(tbuf); return 0; }
            lvl1[idx1] = db;
            if(!WriteBlock(inode->i_block[14], tbuf)){ PageAlloc::DMAAlloc::FreeDMABuffer(tbuf); return 0; }
            // zero new lvl2 block
            PageAlloc::DMAAlloc::DMABuffer* zb = PageAlloc::DMAAlloc::AllocateDMABytes(m_BlockSize);
            if(zb){ String::Memset((void*)zb->VirtAddr,0,m_BlockSize); WriteBlock(db, zb); PageAlloc::DMAAlloc::FreeDMABuffer(zb); }
        }

        U32 lvl2_block = lvl1[idx1];
        PageAlloc::DMAAlloc::FreeDMABuffer(tbuf);

        // read lvl2
        PageAlloc::DMAAlloc::DMABuffer* mbuf = nullptr;
        if(!ReadBlock(lvl2_block, &mbuf)) return 0;
        U32* lvl2 = (U32*)mbuf->VirtAddr; // pointers to single-indirect blocks

        if(lvl2[idx2] == 0){
            U32 sb = AllocateBlock();
            if(sb == 0){ PageAlloc::DMAAlloc::FreeDMABuffer(mbuf); return 0; }
            lvl2[idx2] = sb;
            if(!WriteBlock(lvl2_block, mbuf)){ PageAlloc::DMAAlloc::FreeDMABuffer(mbuf); return 0; }
            // zero new single indirect block
            PageAlloc::DMAAlloc::DMABuffer* zb = PageAlloc::DMAAlloc::AllocateDMABytes(m_BlockSize);
            if(zb){ String::Memset((void*)zb->VirtAddr,0,m_BlockSize); WriteBlock(sb, zb); PageAlloc::DMAAlloc::FreeDMABuffer(zb); }
        }

        U32 target = lvl2[idx2];
        PageAlloc::DMAAlloc::FreeDMABuffer(mbuf);

        // now operate on lvl3 single indirect
        PageAlloc::DMAAlloc::DMABuffer* sbuf = nullptr;
        if(!ReadBlock(target, &sbuf)) return 0;
        U32* pointers = (U32*)sbuf->VirtAddr;
        if(pointers[idx3] == 0){
            U32 newdata = AllocateBlock();
            if(newdata == 0){ PageAlloc::DMAAlloc::FreeDMABuffer(sbuf); return 0; }
            pointers[idx3] = newdata;
            if(!WriteBlock(target, sbuf)){ PageAlloc::DMAAlloc::FreeDMABuffer(sbuf); return 0; }
            // zero new data block
            PageAlloc::DMAAlloc::DMABuffer* dataz = PageAlloc::DMAAlloc::AllocateDMABytes(m_BlockSize);
            if(dataz){ String::Memset((void*)dataz->VirtAddr,0,m_BlockSize); WriteBlock(newdata, dataz); PageAlloc::DMAAlloc::FreeDMABuffer(dataz); }
            inode->i_blocks += (m_BlockSize / 512);
        }

        U32 finalTarget = pointers[idx3];
        PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);
        return finalTarget;
    }

    return 0;
}

// FindInodeForPath implementation (simple, direct blocks only)
    U32 EXT2FileSystem::FindInodeForPath(const char* path, EXT2::Inode* outInode){
        if(!path || String::Strlen(path) == 0) return 0;

        // copy path to mutable buffer for strtok
        CHAR8* path_copy = (CHAR8*)Kmalloc::Alloc(String::Strlen(path) + 1);
        if(!path_copy) return 0;
        String::Strcpy(path_copy, path);

        CHAR8* part = String::Strtok(path_copy, "/");

        // Start at root inode (EXT2 root is typically inode 2)
        U32 current_inode_num = 2;
        EXT2::Inode inode;
        if(!ReadInode(current_inode_num, &inode)){
            Kmalloc::Free(path_copy);
            return 0;
        }

        // If path is just "/" or empty after tokenization
        if(part == nullptr){
            if(outInode) *outInode = inode;
            Kmalloc::Free(path_copy);
            return current_inode_num;
        }

        while(part){
            if(!(inode.i_mode & EXT2_S_IFDIR)){
                // Not a directory, can't traverse
                Kmalloc::Free(path_copy);
                return 0;
            }

            EXT2::DirectoryEntry dirent;
            if(!FindEntryInDirectory(&inode, (const CHAR8*)part, &dirent)){
                // not found
                Kmalloc::Free(path_copy);
                return 0;
            }

            current_inode_num = dirent.inode;
            if(!ReadInode(current_inode_num, &inode)){
                Kmalloc::Free(path_copy);
                return 0;
            }

            part = String::Strtok(nullptr, "/");
        }

        if(outInode) *outInode = inode;
        Kmalloc::Free(path_copy);
        return current_inode_num;
    }