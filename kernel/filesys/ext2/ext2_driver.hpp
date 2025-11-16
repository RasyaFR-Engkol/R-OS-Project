#pragma once
#include "ext2.hpp"
#include <rosval.h>

class Partition;

class EXT2FileSystem : public FileSystem{
    private:
        Partition *m_Partition;
        EXT2::SuperBlock m_Superblock;
        U32 m_BlockSize;
        U32 m_SectorsPerBlock;

        EXT2::BlockGroupDescriptor *m_BGDT;
        U32 m_BGDT_SizeInBlocks;
        U32 m_NumBlockGroups;

        BOOL ReadBlock(U32 blockNum, PageAlloc::DMAAlloc::DMABuffer** outBuffer);
    
        // Membaca Inode
        BOOL ReadInode(U32 inodeNum, EXT2::Inode* outInodeBuffer);

        // Mencari inode dari sebuah path
        U32 FindInodeForPath(const char* path, EXT2::Inode* outInode = nullptr);

        // Dapatkan block disk untuk offset file (implementasi awal)
        U32 GetBlockNumForFileOffset(EXT2::Inode* inode, U32 fileBlockIndex);
        // Get or allocate a block for a given file block index (supports direct + single indirect)
        U32 GetOrAllocateBlockForFileOffset(EXT2::Inode* inode, U32 fileBlockIndex);

        BOOL WriteBlock(U32 blockNum, PageAlloc::DMAAlloc::DMABuffer* buffer);

        U32 AllocateInode();

        U32 AllocateBlock();
        
    // Freeing helpers
    BOOL FreeBlock(U32 BlockNum);
    BOOL FreeInode(U32 InodeNum);
    BOOL PersistSuperblockAndBGDT();

        // Debug instrumentation
        U32 m_DebugFailAfter;
        U32 m_DebugWriteCounter;
    // Debug default owner for created files/dirs. If set to 0xFFFFFFFF, keep original behavior (root)
    U32 m_DebugDefaultUid;
    U32 m_DebugDefaultGid;

        BOOL WriteInode(U32 inodeNum, EXT2::Inode* inode);
        BOOL FindEntryInDirectory(EXT2::Inode *DirInode, const CHAR8 *Name, EXT2::DirectoryEntry *OutEntry = nullptr);
    // If ParentInodeNum != 0, AddEntryToDirectory will persist the parent inode after success
    BOOL AddEntryToDirectory(EXT2::Inode *DirInode, U32 NewInodeNum, const CHAR8* FileName, U16 FileType, U32 ParentInodeNum = 0);
        BOOL RemoveEntryFromDirectory(EXT2::Inode *DirInode, const CHAR8* Name);
    public:
        EXT2FileSystem();
        virtual ~EXT2FileSystem();

        virtual BOOL Mount(Partition *Part) override;
        virtual File* Open(const char* path) override;
        virtual File* Create(const char *Path) override;
        virtual void Close(File* file) override;
        virtual U32 Read(File* file, U8* buffer, U32 size) override; 
        virtual U32 Write(File *File, U8 *Buffer, U32 Size) override;
        virtual BOOL Delete(const char* path) override;
        virtual BOOL Rename(const char* oldPath, const char* newPath) override;
    virtual BOOL Seek(File* file, U64 position) override;
    virtual BOOL Truncate(File* file, U64 size) override;
    virtual BOOL MKDir(const char* path) override;
    virtual BOOL RMDir(const char* path) override;
    virtual BOOL Flush(File* file) override;
    virtual BOOL Append(File* file, U8* buffer, U32 size) override;
        virtual BOOL Cp(const char* srcPath, const char* destPath) override;
        // Debug / testing helpers (not used in normal runtime)
        // Set fail-after N writes for WriteBlock() (0 = disabled)
        void DebugSetFailAfter(U32 writes);
        void DebugResetFail();
        // Run an on-demand consistency scan: check block/inode bitmaps vs actual inode references
        BOOL DebugConsistencyCheck();
    // Attempt a conservative repair of bitmap/inode inconsistencies found by DebugConsistencyCheck
    // Returns number of fixes applied (0 = no fixes)
    U32 DebugRepairConsistency();
    // Forcefully remove a path (debug only). Recursively removes directory contents
    // and frees inodes/blocks regardless of permissions. Use only for recovery/testing.
    BOOL DebugForceRemove(const char* path);
    // Set default owner for newly created inodes (debug helper). Use 0xFFFFFFFF to keep default (root)
    void DebugSetDefaultOwner(U32 uid, U32 gid);
};