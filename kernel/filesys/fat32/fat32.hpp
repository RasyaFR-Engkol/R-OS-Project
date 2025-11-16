#pragma once
#include <rosval.h>

#include <filesystem/filesystem.hpp>
#include "../partition.hpp"

struct FAT32_BPB {
    U8  _jump[3];
    U8  OEMName[8];
    U16 BytesPerSector;
    U8  SectorsPerCluster;
    U16 ReservedSectors;
    U8  NumFATs;
    U16 _rootEntryCount; // (Tidak dipakai di FAT32)
    U16 _totalSectors16; // (Tidak dipakai di FAT32)
    U8  _mediaType;
    U16 _fatSize16;      // (Tidak dipakai di FAT32)
    U16 _sectorsPerTrack;
    U16 _headCount;
    U32 _hiddenSectors;
    U32 TotalSectors32;
    U32 SectorsPerFAT32;
    U16 _extFlags;
    U16 _fsVersion;
    U32 RootDirCluster;
    U16 _fsInfoCluster;
    U16 _backupBootCluster;
    U8  _reserved[12];
    U8  _driveNumber;
    U8  _reserved1;
    U8  _bootSignature;
    U32 _volumeID;
    U8  VolumeLabel[11];
    U8  FSType[8]; // "FAT32   "
} PACKSTRUCT;

struct FAT32_FSInfo {
    U32 LeadSignature;      // 0x000: 0x41615252 ("RRaA")
    U8  Reserved1[480];     // 0x004
    U32 StrucSignature;     // 0x1E4: 0x61417272 ("rrAa")
    U32 FreeClusterCount;   // 0x1E8: Jumlah cluster bebas (0xFFFFFFFF jika tidak diketahui)
    U32 NextFreeCluster;    // 0x1EC: Petunjuk cluster bebas berikutnya
    U8  Reserved2[12];      // 0x1F0
    U32 TrailSignature;     // 0x1FC: 0xAA550000 (tapi dibaca sebagai U32)
} PACKSTRUCT;

struct FAT32_DirectoryEntry {
    U8  Name[11];          // 0x00  Short 8.3 name
    U8  Attributes;        // 0x0B  Attribute flags
    U8  NTReserved;        // 0x0C  Reserved for Windows NT (case info)
    U8  CreationTimeTenth; // 0x0D  Creation time in 10ms units
    U16 CreationTime;      // 0x0E
    U16 CreationDate;      // 0x10
    U16 LastAccessDate;    // 0x12
    U16 ClusterHigh;       // 0x14  High 16 bits of first cluster
    U16 LastWriteTime;     // 0x16
    U16 LastWriteDate;     // 0x18
    U16 ClusterLow;        // 0x1A  Low 16 bits of first cluster
    U32 FileSize;          // 0x1C
} PACKSTRUCT;

// kelas driver FAT32

class FAT32FileSystem : public FileSystem{
    public:
        FAT32FileSystem();
        virtual ~FAT32FileSystem();

        virtual BOOL Mount(Partition *Part) override;
        virtual File *Open(const char* path) override;
        virtual File *Create(const char *path) override;
        virtual void Close(File* file) override;
        virtual U32 Read(File* file, U8* buffer, U32 size) override;
        virtual U32 Write(File *File, U8 *Buffer, U32 Size) override;
        virtual BOOL UpdateDirectoryEntry(File* file) override; // <-- TAMBAHKAN INI
        virtual BOOL Delete(const char* path) override; // Hapus file (non-direktori)
        virtual BOOL Rename(const char* oldPath, const char* newPath) override; // Rename/move file within FAT32
        virtual BOOL Seek(File* file, U64 position) override; // Adjust file position for subsequent Read/Write
        virtual BOOL Truncate(File* file, U64 size) override;
        virtual BOOL MKDir(const char* path) override;
        virtual BOOL RMDir(const char* path) override;
        virtual BOOL Flush(File* file) override;
    virtual BOOL Append(File* file, U8* buffer, U32 size) override;
    virtual BOOL Cp(const char* srcPath, const char* destPath) override;

        Partition* GetPartition() { return m_Partition; }
        U32 GetSectorsPerCluster() { return m_BPB.SectorsPerCluster; }
        U32 GetBytesPerSector() { return m_BPB.BytesPerSector; }
        U64 GetFirstDataSectorLBA() { return m_FirstDataSectorLBA; }

    public:
    // --- Helper Internal ---
    // (Dideklarasikan di sini, didefinisikan di fat32_helper.cpp)
    
    /** @brief Membaca dan memvalidasi BPB. */
    BOOL ParseBPB(U8* bootSector);
    
    /** @brief Mengonversi nomor cluster ke LBA absolut. */
    U64 ClusterToLBA(U32 cluster);
    
    /** @brief Membaca isi FAT untuk mencari cluster selanjutnya. */
    U32 GetNextCluster(U32 currentCluster);

    BOOL SetNextCluster(U32 currentCluster, U32 nextClusterValue);

    U32 FindFreeCluster();

    U32 AllocateCluster(U32 LastCluster);

    VOID ReadFSInfo();

    VOID UpdateFSInfoHint(U32 newHint);
    
    /** @brief Membaca satu cluster penuh ke buffer. */
    BOOL ReadCluster(U32 cluster, U8* buffer);

    BOOL WriteCluster(U32 cluster, U8* buffer);
    
    /** @brief Mencari file di dalam direktori (berdasarkan start cluster dir). */
    // If foundNameOut is non-null, the matched (LFN or SFN) filename will be written to it
    // up to foundNameOutSize bytes (including NUL). If foundNameOut is null, no name is returned.
    BOOL FindFileInDir(const char* name, U32 dirStartCluster, FAT32_DirectoryEntry* entryOut,
                       char* foundNameOut = nullptr, unsigned long long foundNameOutSize = 0,
                       U64* entryLBAOut = nullptr, U32* entryOffsetOut = nullptr);
    
    /** @brief Membaca isi direktori dan memanggil callback untuk setiap entry.
     *  Callback harus mengembalikan TRUE untuk melanjutkan, FALSE untuk berhenti.
     */
    BOOL ReadDirectory(U32 dirStartCluster, 
                       BOOL (*callback)(const char* name, FAT32_DirectoryEntry* de, void* ctx, U64 entryLBA, U32 entryOffset), 
                       void* ctx);

    // Directory listing helper: fills up to maxEntries entries into outEntries and sets outCount.
    // Returns TRUE on success.
    struct DirListingEntry {
        char Name[520];
        FAT32_DirectoryEntry DirEntry;
    };

    BOOL ListDirectory(U32 dirStartCluster, DirListingEntry* outEntries, U32 maxEntries, U32* outCount);

    /** @brief Create short name (SFN) from a UTF-8 filename.
     *  Produces an 11-byte SFN (8+3) in out11. Returns TRUE on success.
     */
    BOOL MakeSFNFromName(const char* name, U8 out11[11]);

    /** @brief Compute SFN checksum used by LFN entries. */
    U8 ComputeSFNChecksum(const U8 sfn[11]);

    /** @brief Create directory entries (LFN + SFN) for 'name' inside dirStartCluster.
     *  If successful, writes the new entries to disk and returns TRUE. If outEntryLBA/Offset
     *  are provided they will hold the LBA/offset of the SFN entry on success.
     */
    BOOL CreateDirectoryEntry(U32 dirStartCluster, const char* name, U32 startCluster, U32 fileSize, U64* outEntryLBA = nullptr, U32* outEntryOffset = nullptr);

    BOOL FreeClusterChain(U32 StartCluster);

private:
    // --- Variabel Internal ---
    Partition* m_Partition;    // Partisi yang kita mount
    FAT32_BPB   m_BPB;          // BPB yang sudah diparsing
    
    U64 m_FirstDataSectorLBA; // LBA awal dari data area (cluster #2)
    U64 m_FirstFATSectorLBA;  // LBA awal dari FAT #1

    U32 m_TotalClusters;       // Total cluster data di partisi
    U32 m_NextFreeClusterHint; // Petunjuk cluster bebas (di-cache dari FSInfo)

    // Internal helper: find contiguous free directory slots
    BOOL FindFreeDirSlots(U32 dirStartCluster, U32 needed, U64* outEntryLBA, U32* outEntryOffset);
};