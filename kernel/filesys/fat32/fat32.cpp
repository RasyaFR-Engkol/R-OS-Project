#include "fat32.hpp"
#include <mm.hpp>       // Untuk PageAlloc::DMAAlloc
#define PRINTK_MODULE_NAME "FAT32"
#include <logging.hpp>  // Untuk Printk
#include <string.hpp>
// Need allocator for temporary buffers
#include "../../mm/kmalloc/kmalloc.hpp"

FAT32FileSystem::FAT32FileSystem(){
    String::Memset(&m_BPB, 0, sizeof(FAT32_BPB));
    m_FirstDataSectorLBA = 0;
    m_FirstFATSectorLBA = 0;
}

FAT32FileSystem::~FAT32FileSystem(){};

BOOL FAT32FileSystem::Mount(Partition *Part){
    m_Partition = Part;
    if(!m_Partition) return FALSE;
    
    PageAlloc::DMAAlloc::DMABuffer *Buffer = nullptr;
    if(!m_Partition->ReadSectors(0, 1, &Buffer)){
        Printk::Write(Printk::Level::LOG_ERR, "FAT32: Failed to Read Sector for mount\n");
        return FALSE;
    }

    BOOL BPBOK = this->ParseBPB((U8*)Buffer->VirtAddr);
    PageAlloc::DMAAlloc::FreeDMABuffer(Buffer);

    if(!BPBOK){
        Printk::Write(Printk::Level::LOG_ERR, "Invalid FAT32 BPB.\n");
        m_Partition = nullptr;
        return FALSE;
    }

    m_FirstFATSectorLBA = m_BPB.ReservedSectors;
    U64 SectorsPerFat = (U64)m_BPB.SectorsPerFAT32;
    m_FirstDataSectorLBA = m_BPB.ReservedSectors + ((U64)m_BPB.NumFATs * SectorsPerFat);

    U64 DataSectorCount = (U64)m_BPB.TotalSectors32 - m_FirstDataSectorLBA;
    m_TotalClusters = (U32)(DataSectorCount / m_BPB.SectorsPerCluster);

    m_NextFreeClusterHint = 2; // start searching from cluster 2

    ReadFSInfo();

    Printk::Write(Printk::Level::LOG_INFO, "FAT32: Mount OK. DataLBA: %llu, TotalClusters: %u, NextFreeHint: %u",
        m_FirstDataSectorLBA, m_TotalClusters, m_NextFreeClusterHint);
    Printk::Write(Printk::Level::LOG_INFO, "  Root Cluster: %d\n", m_BPB.RootDirCluster);
    
    
    return TRUE;
}

File* FAT32FileSystem::Open(const char* path){
    if(!m_Partition) return nullptr;

    // Support opening root directory
    if(path && path[0] == '/' && path[1] == '\0'){
        File* froot = new File();
        // Do not memset the whole object: that would overwrite the vptr and
        // lead to a null-deref when deleting the object. Initialize fields
        // explicitly instead.
        froot->FileSize = 0;
        froot->CurrentPosition = 0;
        froot->IsDirectory = TRUE;
        froot->Internal_StartCluster = m_BPB.RootDirCluster;
        froot->Internal_CurrentCluster = m_BPB.RootDirCluster;
        froot->FileName[0] = '\0';
        String::Strcpy(froot->FileName, "/");
        
        // --- PERUBAHAN ---
        // Root dir tidak memiliki entri standar, jadi LBA/offset-nya 0
        froot->Internal_DirEntryLBA = 0;
        froot->Internal_DirEntryOffset = 0;
        froot->FSOwner = this;
        // --- AKHIR PERUBAHAN ---

        return froot;
    }

    // Support path traversal with '/' separators. Start at root cluster.
    U32 currentCluster = m_BPB.RootDirCluster;
    const char* p = path;
    char comp[260];
    char foundNameLocal[520];
    FAT32_DirectoryEntry entry;

    // --- PERUBAHAN ---
    // Variabel untuk menyimpan lokasi entri *final*
    U64 finalEntryLBA = 0;
    U32 finalEntryOffset = 0;
    // --- AKHIR PERUBAHAN ---

    const char* segStart = p;
    while(true){
        if(!segStart || *segStart == '\0') break;
        // find next slash
        const char* slash = String::Strchr(segStart, '/');
        unsigned long long seglen = slash ? (unsigned long long)(slash - segStart) : String::Strlen(segStart);
        if(seglen == 0){ // skip empty component (e.g., leading or double slash)
            segStart = slash ? slash + 1 : nullptr;
            if(!segStart) break;
            continue;
        }
        if(seglen >= sizeof(comp)) seglen = sizeof(comp)-1;
        // copy component
        for(unsigned long long i=0;i<seglen;i++) comp[i] = segStart[i];
        comp[seglen] = '\0';

        // ask FindFileInDir to also return the matched filename (LFN if present)
        foundNameLocal[0] = '\0';

        // --- PERUBAHAN ---
        // Variabel untuk LBA/offset komponen *saat ini*
        U64 currentEntryLBA = 0;
        U32 currentEntryOffset = 0;
        
        // Panggil FindFileInDir dengan TANDA TANGAN BARU
        if(!FindFileInDir(comp, currentCluster, &entry, 
                         foundNameLocal, sizeof(foundNameLocal), 
                         &currentEntryLBA, &currentEntryOffset)) // <-- Argumen baru
        {
            Printk::Write(Printk::Level::LOG_WARNING, "FAT32: Path component '%s' not found\n", comp);
            return nullptr;
        }
        // --- AKHIR PERUBAHAN ---


        // If there's another segment, the found entry must be a directory
        if(slash){
            if(!(entry.Attributes & 0x10)){
                Printk::Write(Printk::Level::LOG_WARNING, "FAT32: Path component '%s' is not a directory\n", comp);
                return nullptr;
            }
            // advance into directory
            currentCluster = ((U32)entry.ClusterHigh << 16) | (U32)entry.ClusterLow;
            segStart = slash + 1;
            continue;
        } else {
            // last component, entry contains the target. 
            // --- PERUBAHAN ---
            // Simpan LBA/offset dari entri *terakhir* ini
            finalEntryLBA = currentEntryLBA;
            finalEntryOffset = currentEntryOffset;
            // --- AKHIR PERUBAHAN ---
            break;
        }
    }

    // Build File struct from entry
    File* f = new File();
    // Initialize fields explicitly to avoid wiping the vptr via memset
    f->FileSize = entry.FileSize;
    f->CurrentPosition = 0;
    f->IsDirectory = (entry.Attributes & 0x10) ? TRUE : FALSE;
    U32 startCluster = ((U32)entry.ClusterHigh << 16) | (U32)entry.ClusterLow;
    f->Internal_StartCluster = startCluster;
    f->Internal_CurrentCluster = startCluster;

    // --- PERUBAHAN ---
    // Simpan lokasi entri di disk ke struct File
    f->Internal_DirEntryLBA = finalEntryLBA;
    f->Internal_DirEntryOffset = finalEntryOffset;
    // Pastikan FSOwner diisi agar operasi VFS (Read/Write/Close/Seek) berjalan
    f->FSOwner = this;
    // --- AKHIR PERUBAHAN ---

    // Build file name: prefer LFN if available (returned in foundNameLocal), otherwise use SFN.
    if(foundNameLocal[0] != '\0'){
        // copy LFN into FileName (truncate if needed)
        unsigned long long tocpy = String::Strlen(foundNameLocal);
        if(tocpy >= sizeof(f->FileName)) tocpy = sizeof(f->FileName) - 1;
        String::Memcpy(f->FileName, foundNameLocal, tocpy);
        f->FileName[tocpy] = '\0';
    } else {
        char namebuf[13]; int ni=0;
        for(int i=0;i<8;i++){
            char c = entry.Name[i]; if(c == ' ') break; namebuf[ni++] = c;
        }
        // extension
        int extlen = 0;
        for(int i=8;i<11;i++) if(entry.Name[i] != ' ') extlen++;
        if(extlen > 0){ namebuf[ni++] = '.'; for(int i=8;i<11;i++){ if(entry.Name[i] != ' ') namebuf[ni++] = entry.Name[i]; } }
        namebuf[ni] = '\0';
        String::Memcpy(f->FileName, namebuf, ni+1);
    }

    return f;
}

void FAT32FileSystem::Close(File* file){
    if(!file) return;
    delete file;
}

U32 FAT32FileSystem::Read(File* file, U8* buffer, U32 size){
    Printk::Write(Printk::Level::LOG_INFO, "FAT32: Read requested, size %u bytes\n", size);
    if(!file || !buffer || size == 0) return 0;
    if(file->CurrentPosition >= file->FileSize) return 0;

    U64 bytesPerCluster = (U64)m_BPB.BytesPerSector * (U64)m_BPB.SectorsPerCluster;
    U64 remainingFile = file->FileSize - file->CurrentPosition;
    U32 toRead = (U32)((size > remainingFile) ? remainingFile : size);
    U32 totalRead = 0;

    // Determine starting cluster and offset
    U32 clusterIndex = file->CurrentPosition / bytesPerCluster;
    U32 offsetInCluster = file->CurrentPosition % bytesPerCluster;

    U32 cluster = file->Internal_StartCluster;
    // Walk to the clusterIndex
    for(U32 i = 0; i < clusterIndex; i++){
        cluster = GetNextCluster(cluster);
        if(cluster >= 0x0FFFFFF8) return totalRead; // end of chain
    }

    while(toRead > 0){
        // Read current cluster
        PageAlloc::DMAAlloc::DMABuffer* buf = nullptr;
        U64 lba = ClusterToLBA(cluster);
        if(!m_Partition->ReadSectors(lba, m_BPB.SectorsPerCluster, &buf)){
            Printk::Write(Printk::Level::LOG_ERR, "FAT32: Failed to read cluster %u\n", cluster);
            break;
        }

        U8* clusterData = (U8*)buf->VirtAddr;
        U32 avail = (U32)(bytesPerCluster - offsetInCluster);
        U32 want = (toRead < avail) ? toRead : avail;
        String::Memcpy(buffer + totalRead, clusterData + offsetInCluster, want);
        PageAlloc::DMAAlloc::FreeDMABuffer(buf);

        totalRead += want;
        toRead -= want;
        offsetInCluster = 0; // subsequent clusters start at 0

        if(toRead == 0) break;

        // Move to next cluster in chain
        cluster = GetNextCluster(cluster);
        if(cluster >= 0x0FFFFFF8) break; // end of chain
    }

    file->CurrentPosition += totalRead;
    file->Internal_CurrentCluster = cluster;
    return totalRead;
}

U32 FAT32FileSystem::Write(File* file, U8* buffer, U32 size){
    Printk::Write(Printk::Level::LOG_INFO, "FAT32: Write requested, size %u bytes\n", size);
    if(!file || !buffer || size == 0) return 0;

    // Only write to existing files. If the file has no directory entry (Internal_DirEntryLBA==0)
    // we treat it as non-creatable in this implementation and skip.
    if(file->Internal_DirEntryLBA == 0 && file->Internal_DirEntryOffset == 0){
        // It may be the root directory (which has LBA 0 in our structure). Do not write directories.
        if(file->IsDirectory) return 0;
        // If file has no directory entry recorded, we skip as requested
        return 0;
    }

    U64 clusterBytes = (U64)m_BPB.BytesPerSector * (U64)m_BPB.SectorsPerCluster;
    if(clusterBytes == 0) return 0;

    U64 startPos = (U64)file->CurrentPosition; // seek position
    U32 totalWritten = 0;

    // Ensure we have a start cluster; if not, allocate one (existing file with zero-length)
    U32 cluster = file->Internal_StartCluster;
    if(cluster == 0){
        U32 nc = AllocateCluster(0);
        if(nc == 0) return 0;
        file->Internal_StartCluster = nc;
        cluster = nc;
    }

    // Walk to cluster that contains startPos
    U64 clusterIndex = startPos / clusterBytes;
    U32 cur = cluster;
    U32 prev = 0;
    for(U64 i = 0; i < clusterIndex; ++i){
        prev = cur;
        U32 next = GetNextCluster(cur);
        if(next >= 0x0FFFFFF8){
            // allocate and link a new cluster
            U32 newc = AllocateCluster(cur);
            if(newc == 0) {
                // unable to allocate
                file->CurrentPosition = (U32)startPos; // keep position conservative
                return totalWritten;
            }
            cur = newc;
        } else {
            cur = next;
        }
    }
    cluster = cur;

    // offset in first cluster
    U32 offsetInCluster = (U32)(startPos % clusterBytes);

    U32 remaining = size;
    U8* srcPtr = buffer;

    while(remaining > 0){
        // If cluster is 0/invalid, allocate
        if(cluster == 0){
            U32 newc = AllocateCluster(prev);
            if(newc == 0) break;
            cluster = newc;
        }

        // compute write size in this cluster
        U32 can = (U32)(clusterBytes - offsetInCluster);
        U32 want = (remaining < can) ? remaining : can;

        if(offsetInCluster == 0 && want == clusterBytes){
            // full-cluster write: write directly from caller buffer
            // WriteCluster will allocate DMA buffer and write
            if(!WriteCluster(cluster, srcPtr)){
                Printk::Write(Printk::Level::LOG_ERR, "FAT32: WriteCluster failed for cluster %u\n", cluster);
                break;
            }
        } else {
            // partial cluster: read current cluster into temp, patch, then write whole cluster
            U32 cbytes = (U32)clusterBytes;
            U8* tmp = (U8*)Kmalloc::Alloc(cbytes);
            if(!tmp){
                Printk::Write(Printk::Level::LOG_ERR, "FAT32: temporary alloc failed for write\n");
                break;
            }
            // If cluster is not yet allocated (value == 0), the read may fail; zero buffer
            if(!ReadCluster(cluster, tmp)){
                // zero-fill then write
                for(U32 z=0; z<cbytes; ++z) tmp[z] = 0;
            }
            // copy 'want' bytes
            String::Memcpy(tmp + offsetInCluster, srcPtr, want);

            if(!WriteCluster(cluster, tmp)){
                Printk::Write(Printk::Level::LOG_ERR, "FAT32: WriteCluster failed for cluster %u (partial)\n", cluster);
                Kmalloc::Free(tmp);
                break;
            }
            Kmalloc::Free(tmp);
        }

        // advance
        totalWritten += want;
        remaining -= want;
        srcPtr += want;
        offsetInCluster = 0; // subsequent clusters start at 0

        // move to next cluster or allocate if needed
        prev = cluster;
        U32 nextc = GetNextCluster(cluster);
        if(remaining > 0){
            if(nextc >= 0x0FFFFFF8){
                // allocate and link
                U32 newc = AllocateCluster(cluster);
                if(newc == 0) break;
                cluster = newc;
            } else {
                cluster = nextc;
            }
        }
    }

    // Update file size and current position
    U64 newPos = startPos + (U64)totalWritten;
    if(newPos > file->FileSize) file->FileSize = newPos;
    // clamp CurrentPosition to 32-bit (existing API uses U32)
    file->CurrentPosition = (U32)newPos;
    file->Internal_CurrentCluster = cluster;

    // Persist directory entry
    if(!UpdateDirectoryEntry(file)){
        Printk::Write(Printk::Level::LOG_WARNING, "FAT32: UpdateDirectoryEntry failed after write\n");
    }

    Printk::Write(Printk::Level::LOG_INFO, "FAT32: Write completed, %u bytes written\n", totalWritten);

    return totalWritten;
}

File *FAT32FileSystem::Create(const char *path){
    if(!path || path[0] != '/') return nullptr; // Hanya support full path
    Printk::Write(Printk::Level::LOG_DEBUG, "FAT32: Create called for '%s'\n", path);
    
    // 1. Cek dulu apakah file sudah ada
    // Open() akan mengembalikan non-null jika ada
    File* existing = Open(path);
    if(existing){
        Close(existing); // Tutup lagi
        Printk::Write(Printk::Level::LOG_WARNING, "FAT32: Create failed, file '%s' is exist", path);
        return nullptr; // File sudah ada
    }

    // 2. Pisahkan path: "/folder/baru/file.txt" -> "/folder/baru" dan "file.txt"
    char parentPath[256];
    const char* newName = nullptr;
    size_t len = String::Strlen(path);
    int lastSlash = -1;

    /* Scan backwards without signed overflow by using an unsigned
       size_t loop index and decrementing until zero. */
    if(len > 0){
        for(size_t ui = len; ui > 0; ){ 
            ui--;
            if(path[ui] == '/'){
                lastSlash = (int)ui;
                break;
            }
        }
    }

    if(lastSlash == -1) return nullptr; // Seharusnya tidak terjadi jika path[0] == '/'

    if(lastSlash == 0){ // File di root, e.g. "/file.txt"
        String::Strcpy(parentPath, "/");
        newName = path + 1;
    } else { // File di subfolder, e.g. "/folder/file.txt"
        String::Memcpy(parentPath, path, (U64)lastSlash);
        parentPath[lastSlash] = '\0';
        newName = path + lastSlash + 1;
    }

    if(String::Strlen(newName) == 0) return nullptr; // Tidak boleh nama kosong

    // 3. Buka direktori induk
    File* parentDir = Open(parentPath);
    if(!parentDir){
        Printk::Write(Printk::Level::LOG_ERR, "FAT32: Create gagal, folder induk '%s' tidak ada", parentPath);
        return nullptr;
    }
    if(!parentDir->IsDirectory){
        Printk::Write(Printk::Level::LOG_ERR, "FAT32: Create gagal, '%s' bukan direktori", parentPath);
        Close(parentDir);
        return nullptr;
    }

    // 4. Panggil helper untuk membuat entri (LFN + SFN) di disk
    U32 parentCluster = parentDir->Internal_StartCluster;
    
    // File baru punya start cluster 0 dan size 0
    BOOL created = CreateDirectoryEntry(parentCluster, newName, 0, 0); 
    
    Close(parentDir); // Selesai dengan direktori induk

    if(!created){
        Printk::Write(Printk::Level::LOG_ERR, "FAT32: CreateDirectoryEntry gagal untuk '%s' (parentCluster=%u)\n", newName, parentCluster);
        return nullptr;
    }

    Printk::Write(Printk::Level::LOG_INFO, "FAT32: File '%s' created successfully\n", path);

    // 5. Sukses! Entri sudah ada di disk.
    // Cara termudah dan teraman untuk dapat File* adalah... panggil Open() lagi.
    // Ini akan membaca entri yang baru saja kita tulis.
    return Open(path);
}

// Copy a file within FAT32 filesystem. srcPath and destPath should be absolute within FS (start with '/')
BOOL FAT32FileSystem::Cp(const char* srcPath, const char* destPath){
    if(!srcPath || !destPath) return FALSE;
    File* src = Open(srcPath);
    if(!src) return FALSE;
    if(src->IsDirectory){ Close(src); return FALSE; }

    File* dst = Create(destPath);
    if(!dst){ Close(src); return FALSE; }

    // buffer size: use cluster size if available, else 4096
    U32 clusterBytes = (U32)m_BPB.BytesPerSector * (U32)m_BPB.SectorsPerCluster;
    U32 bufSize = (clusterBytes > 0 && clusterBytes <= 65536) ? clusterBytes : 4096;
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

// helper functions moved to fat32_helper.cpp

BOOL FAT32FileSystem::Delete(const char* path){
    if(!m_Partition || !path || path[0] != '/') return FALSE;

    // 1) Split parent directory and filename
    char parentPath[256];
    const char* name = nullptr;
    size_t len = String::Strlen(path);
    int lastSlash = -1;
    if(len > 0){
        for(size_t ui = len; ui > 0; ){ ui--; if(path[ui] == '/') { lastSlash = (int)ui; break; } }
    }
    if(lastSlash < 0) return FALSE;

    if(lastSlash == 0){
        String::Strcpy(parentPath, "/");
        name = path + 1;
    } else {
        if((size_t)lastSlash >= sizeof(parentPath)) return FALSE;
        String::Memcpy(parentPath, path, (U64)lastSlash);
        parentPath[lastSlash] = '\0';
        name = path + lastSlash + 1;
    }

    if(!name || String::Strlen(name) == 0) return FALSE;

    // 2) Open parent directory
    File* parentDir = Open(parentPath);
    if(!parentDir || !parentDir->IsDirectory){ if(parentDir) Close(parentDir); return FALSE; }

    U32 dirStartCluster = parentDir->Internal_StartCluster;

    // 3) Find the file entry in the directory
    FAT32_DirectoryEntry de;
    U64 entryLBA = 0; U32 entryOffset = 0;
    if(!FindFileInDir(name, dirStartCluster, &de, nullptr, 0, &entryLBA, &entryOffset)){
        Close(parentDir);
        return FALSE; // not found
    }

    // Don't delete directories in this function
    if(de.Attributes & 0x10){
        Close(parentDir);
        return FALSE;
    }

    // 4) Free the cluster chain for the file
    U32 startCluster = ((U32)de.ClusterHigh << 16) | (U32)de.ClusterLow;
    if(startCluster >= 2) {
        FreeClusterChain(startCluster);
    }

    // 5) Mark SFN entry as deleted (0xE5 as first byte)
    PageAlloc::DMAAlloc::DMABuffer* sbuf = nullptr;
    if(!m_Partition->ReadSectors(entryLBA, 1, &sbuf)){
        Close(parentDir);
        return FALSE;
    }
    U8* sdata = (U8*)sbuf->VirtAddr;
    sdata[entryOffset + 0] = 0xE5;
    BOOL wrote = m_Partition->WriteSectors(entryLBA, 1, sbuf);
    PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);
    if(!wrote){ Close(parentDir); return FALSE; }

    // 6) Also mark any preceding LFN entries as deleted
    // Build the ordered list of sector LBAs that make up this directory
    U32 sectorsPerCluster = m_BPB.SectorsPerCluster;

    // First pass: count clusters and sectors
    U32 clCount = 0;
    {
        U32 c = dirStartCluster;
        U32 guard = 0;
        while(c >= 2 && c < 0x0FFFFFF8){
            clCount++;
            c = GetNextCluster(c);
            if(++guard > m_TotalClusters + 2) break; // avoid infinite loop on corrupt FAT
        }
    }
    U64 totalSectors = (U64)clCount * (U64)sectorsPerCluster;
    if(totalSectors == 0){ Close(parentDir); return TRUE; }

    U64* sectorLBAs = (U64*)Kmalloc::Alloc((U32)(totalSectors * sizeof(U64)));
    if(!sectorLBAs){ Close(parentDir); return TRUE; }

    // Second pass: fill LBAs
    {
        U32 c = dirStartCluster; U64 idx = 0; U32 guard = 0;
        while(c >= 2 && c < 0x0FFFFFF8){
            U64 base = ClusterToLBA(c);
            for(U32 s=0; s<sectorsPerCluster; ++s){ sectorLBAs[idx++] = base + s; }
            c = GetNextCluster(c);
            if(++guard > m_TotalClusters + 2) break;
        }
    }

    // Locate the sector index of the SFN entry
    long long sfnIdx = -1;
    for(U64 i=0;i<totalSectors;i++){ if(sectorLBAs[i] == entryLBA){ sfnIdx = (long long)i; break; } }

    if(sfnIdx >= 0){
        // Helper to load and optionally flush a sector buffer
        PageAlloc::DMAAlloc::DMABuffer* curBuf = nullptr; U64 curLBA = (U64)-1; BOOL dirty = FALSE;
        auto loadSector = [&](U64 lba)->BOOL{
            if(curLBA == lba && curBuf) return TRUE;
            if(curBuf){ if(dirty){ if(!m_Partition->WriteSectors(curLBA, 1, curBuf)){ PageAlloc::DMAAlloc::FreeDMABuffer(curBuf); curBuf=nullptr; dirty=FALSE; return FALSE; } dirty = FALSE; } PageAlloc::DMAAlloc::FreeDMABuffer(curBuf); curBuf=nullptr; }
            if(!m_Partition->ReadSectors(lba, 1, &curBuf)) return FALSE;
            curLBA = lba; dirty = FALSE; return TRUE;
        };
        auto flushSector = [&]()->BOOL{
            if(curBuf && dirty){ if(!m_Partition->WriteSectors(curLBA, 1, curBuf)) return FALSE; dirty = FALSE; }
            return TRUE;
        };

        long long secIdx = sfnIdx;
        U32 offs = entryOffset;
        if(loadSector(sectorLBAs[secIdx])){
            // Walk backwards over 32-byte entries before SFN, deleting LFN entries
            while(true){
                if(offs >= 32){ offs -= 32; }
                else {
                    if(secIdx == 0) break; // reached start of directory
                    if(!flushSector()) break;
                    secIdx -= 1; offs = (U32)m_BPB.BytesPerSector - 32;
                    if(!loadSector(sectorLBAs[secIdx])) break;
                }

                U8* d = (U8*)curBuf->VirtAddr;
                U8 first = d[offs + 0];
                U8 attr  = d[offs + 11];
                if(first == 0x00){
                    // Unused entry; nothing more to delete backwards
                    break;
                }
                if(attr != 0x0F){
                    // Not an LFN entry; we've finished deleting preceding LFNs
                    break;
                }
                // Mark this LFN entry deleted
                d[offs + 0] = 0xE5; dirty = TRUE;
            }
            flushSector();
        }
        if(curBuf) PageAlloc::DMAAlloc::FreeDMABuffer(curBuf);
    }

    Kmalloc::Free(sectorLBAs);
    Close(parentDir);
    return TRUE;
}

BOOL FAT32FileSystem::Rename(const char* oldPath, const char* newPath){
    if(!m_Partition || !oldPath || !newPath) return FALSE;
    if(oldPath[0] != '/' || newPath[0] != '/') return FALSE;

    // Parse old path parent and name
    char oldParent[256]; const char* oldName = nullptr;
    size_t olen = String::Strlen(oldPath); int oslash = -1;
    if(olen > 0){ for(size_t i=olen; i>0; ){ i--; if(oldPath[i]=='/'){ oslash=(int)i; break; } } }
    if(oslash < 0) return FALSE;
    if(oslash == 0){ String::Strcpy(oldParent, "/"); oldName = oldPath + 1; }
    else { if((size_t)oslash >= sizeof(oldParent)) return FALSE; String::Memcpy(oldParent, oldPath, (U64)oslash); oldParent[oslash]='\0'; oldName = oldPath + oslash + 1; }
    if(!oldName || String::Strlen(oldName)==0) return FALSE;

    // Parse new path parent and name
    char newParent[256]; const char* newName = nullptr;
    size_t nlen = String::Strlen(newPath); int nslash = -1;
    if(nlen > 0){ for(size_t i=nlen; i>0; ){ i--; if(newPath[i]=='/'){ nslash=(int)i; break; } } }
    if(nslash < 0) return FALSE;
    if(nslash == 0){ String::Strcpy(newParent, "/"); newName = newPath + 1; }
    else { if((size_t)nslash >= sizeof(newParent)) return FALSE; String::Memcpy(newParent, newPath, (U64)nslash); newParent[nslash]='\0'; newName = newPath + nslash + 1; }
    if(!newName || String::Strlen(newName)==0) return FALSE;

    // Open old parent and find entry
    File* oldDir = Open(oldParent);
    if(!oldDir || !oldDir->IsDirectory){ if(oldDir) Close(oldDir); return FALSE; }
    U32 oldDirCl = oldDir->Internal_StartCluster;

    FAT32_DirectoryEntry oldDe; U64 oldLBA=0; U32 oldOff=0;
    if(!FindFileInDir(oldName, oldDirCl, &oldDe, nullptr, 0, &oldLBA, &oldOff)){
        Close(oldDir); return FALSE;
    }
    // For now, do not rename directories
    if(oldDe.Attributes & 0x10){ Close(oldDir); return FALSE; }

    // If destination exists, fail
    File* newDir = Open(newParent);
    if(!newDir || !newDir->IsDirectory){ if(newDir) Close(newDir); Close(oldDir); return FALSE; }
    U32 newDirCl = newDir->Internal_StartCluster;

    FAT32_DirectoryEntry tmpDe; U64 tmpLBA; U32 tmpOff;
    if(FindFileInDir(newName, newDirCl, &tmpDe, nullptr, 0, &tmpLBA, &tmpOff)){
        // name exists
        Close(newDir); Close(oldDir); return FALSE;
    }

    // Create a new entry at destination with same start cluster and size
    U32 startCluster = ((U32)oldDe.ClusterHigh << 16) | (U32)oldDe.ClusterLow;
    if(!CreateDirectoryEntry(newDirCl, newName, startCluster, oldDe.FileSize)){
        Close(newDir); Close(oldDir); return FALSE;
    }

    // Delete old entry (SFN + preceding LFN entries), but do NOT free clusters
    // Mark SFN deleted
    PageAlloc::DMAAlloc::DMABuffer* sbuf = nullptr;
    if(!m_Partition->ReadSectors(oldLBA, 1, &sbuf)){
        Close(newDir); Close(oldDir); return FALSE;
    }
    U8* sdata = (U8*)sbuf->VirtAddr;
    sdata[oldOff + 0] = 0xE5;
    BOOL wrote = m_Partition->WriteSectors(oldLBA, 1, sbuf);
    PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);
    if(!wrote){ Close(newDir); Close(oldDir); return FALSE; }

    // Delete preceding LFN entries in old parent directory
    // Build ordered LBA list for the old directory
    U32 spc = m_BPB.SectorsPerCluster;
    U32 bps = m_BPB.BytesPerSector;
    // Count clusters
    U32 clCount = 0; { U32 c=oldDirCl, guard=0; while(c>=2 && c<0x0FFFFFF8){ clCount++; c=GetNextCluster(c); if(++guard>m_TotalClusters+2) break; } }
    U64 totalSecs = (U64)clCount * (U64)spc;
    if(totalSecs > 0){
        U64* lbas = (U64*)Kmalloc::Alloc((U32)(totalSecs * sizeof(U64)));
        if(lbas){
            // fill
            { U32 c=oldDirCl, guard=0; U64 idx=0; while(c>=2 && c<0x0FFFFFF8){ U64 base=ClusterToLBA(c); for(U32 s=0;s<spc;++s) lbas[idx++]=base+s; c=GetNextCluster(c); if(++guard>m_TotalClusters+2) break; } }
            // find sector index
            long long sfnIdx = -1; for(U64 i=0;i<totalSecs;i++){ if(lbas[i]==oldLBA){ sfnIdx=(long long)i; break; } }
            if(sfnIdx >= 0){
                PageAlloc::DMAAlloc::DMABuffer* curBuf=nullptr; U64 curLBA=(U64)-1; BOOL dirty=FALSE;
                auto load = [&](U64 lba)->BOOL{ if(curLBA==lba && curBuf) return TRUE; if(curBuf){ if(dirty){ if(!m_Partition->WriteSectors(curLBA,1,curBuf)){ PageAlloc::DMAAlloc::FreeDMABuffer(curBuf); curBuf=nullptr; dirty=FALSE; return FALSE; } dirty=FALSE; } PageAlloc::DMAAlloc::FreeDMABuffer(curBuf); curBuf=nullptr; } return m_Partition->ReadSectors(lba,1,&curBuf) ? (curLBA=lba, TRUE) : FALSE; };
                auto flush = [&]()->BOOL{ if(curBuf && dirty){ if(!m_Partition->WriteSectors(curLBA,1,curBuf)) return FALSE; dirty=FALSE; } return TRUE; };
                long long secIdx = sfnIdx; U32 offs = oldOff;
                if(load(lbas[secIdx])){
                    while(true){
                        if(offs >= 32) offs -= 32; else { if(secIdx==0) break; if(!flush()) break; secIdx -= 1; offs = bps - 32; if(!load(lbas[secIdx])) break; }
                        U8* d=(U8*)curBuf->VirtAddr; U8 first=d[offs+0]; U8 attr=d[offs+11];
                        if(first==0x00) break; // end of used entries
                        if(attr != 0x0F) break; // stop at first non-LFN
                        d[offs+0]=0xE5; dirty=TRUE; // delete LFN entry
                    }
                    flush();
                }
                if(curBuf) PageAlloc::DMAAlloc::FreeDMABuffer(curBuf);
            }
            Kmalloc::Free(lbas);
        }
    }

    Close(newDir);
    Close(oldDir);
    return TRUE;
}

BOOL FAT32FileSystem::Seek(File* file, U64 position){
    if(!file) return FALSE;
    // Directories: do not support seeking for now
    if(file->IsDirectory) return FALSE;

    // Clamp position within [0, FileSize]
    if(position > file->FileSize) position = file->FileSize;

    // Fast path: if seeking to current, nothing to do
    if((U64)file->CurrentPosition == position) return TRUE;

    U64 clusterBytes = (U64)m_BPB.BytesPerSector * (U64)m_BPB.SectorsPerCluster;
    if(clusterBytes == 0) return FALSE;

    // Determine target cluster index and offset
    U64 targetClusterIndex = position / clusterBytes;
    U32 offsetInCluster = (U32)(position % clusterBytes);

    // Walk from start of chain to target cluster; this is O(n) but acceptable for now
    U32 cluster = file->Internal_StartCluster;
    if(cluster == 0 && position != 0){
        // no cluster allocated yet but seeking beyond 0 is invalid for empty files
        return FALSE;
    }

    for(U64 i = 0; i < targetClusterIndex; ++i){
        cluster = GetNextCluster(cluster);
        if(cluster >= 0x0FFFFFF8){
            // reached end of chain before intended position; clamp to EOF
            file->CurrentPosition = (U32)file->FileSize;
            file->Internal_CurrentCluster = cluster;
            return TRUE;
        }
    }

    file->CurrentPosition = (U32)position;
    file->Internal_CurrentCluster = cluster;
    (void)offsetInCluster; // stored implicitly via CurrentPosition
    return TRUE;
}

BOOL FAT32FileSystem::Truncate(File* file, U64 size){
    if(!file) return FALSE;
    if(file->IsDirectory) return FALSE;

    U64 clusterBytes = (U64)m_BPB.BytesPerSector * (U64)m_BPB.SectorsPerCluster;
    if(clusterBytes == 0) return FALSE;

    U64 oldSize = file->FileSize;
    if(size == oldSize) return TRUE;

    // Shrink
    if(size < oldSize){
        U32 keepClusters = (size == 0) ? 0 : (U32)((size + clusterBytes - 1) / clusterBytes);

        U32 start = file->Internal_StartCluster;
        if(keepClusters == 0){
            if(start >= 2){
                FreeClusterChain(start);
            }
            file->Internal_StartCluster = 0;
            file->Internal_CurrentCluster = 0;
            file->FileSize = (U32)size;
            if((U64)file->CurrentPosition > size) file->CurrentPosition = (U32)size;
            UpdateDirectoryEntry(file);
            return TRUE;
        }

        // walk to the last cluster to keep
        U32 cur = start;
        if(cur < 2) return FALSE; // inconsistent
        for(U32 i = 0; i < keepClusters - 1; ++i){
            U32 nxt = GetNextCluster(cur);
            if(nxt >= 0x0FFFFFF8) break;
            cur = nxt;
        }

        U32 toFree = GetNextCluster(cur);
        if(toFree >= 2 && toFree < 0x0FFFFFF8){
            FreeClusterChain(toFree);
        }

        // mark cur as end-of-chain
        SetNextCluster(cur, 0x0FFFFFFF);

        file->FileSize = (U32)size;
        if((U64)file->CurrentPosition > size) file->CurrentPosition = (U32)size;
        file->Internal_CurrentCluster = cur;
        UpdateDirectoryEntry(file);
        return TRUE;
    }

    // Extend: allocate clusters and zero them
    U32 oldClusters = (oldSize == 0) ? 0 : (U32)((oldSize + clusterBytes - 1) / clusterBytes);
    U32 newClusters = (size == 0) ? 0 : (U32)((size + clusterBytes - 1) / clusterBytes);
    if(newClusters <= oldClusters){
        // file size grows within existing cluster
        file->FileSize = (U32)size;
        UpdateDirectoryEntry(file);
        return TRUE;
    }

    U32 need = newClusters - oldClusters;
    U32 last = file->Internal_StartCluster;

    // If no start cluster, allocate first
    if(last < 2){
        U32 nc = AllocateCluster(0);
        if(nc == 0) return FALSE;
        // ensure chain termination
        SetNextCluster(nc, 0x0FFFFFFF);
        last = nc;

        // zero new cluster
        U32 bytes = (U32)clusterBytes;
        U32 pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
        PageAlloc::DMAAlloc::DMABuffer* zbuf = PageAlloc::DMAAlloc::AllocateDMAPages(pages);
        if(!zbuf) return FALSE;
        String::Memset((U8*)zbuf->VirtAddr, 0, bytes);
        WriteCluster(nc, (U8*)zbuf->VirtAddr);
        PageAlloc::DMAAlloc::FreeDMABuffer(zbuf);

        need--;
    }

    // walk to end of chain
    U32 guard = 0;
    U32 cur = last;
    while(true){
        U32 nxt = GetNextCluster(cur);
        if(nxt >= 0x0FFFFFF8) break;
        cur = nxt;
        if(++guard > m_TotalClusters + 2) break;
    }
    last = cur;

    for(U32 i = 0; i < need; ++i){
        U32 nc = AllocateCluster(last);
        if(nc == 0) return FALSE;
        // mark new cluster EOC
        SetNextCluster(nc, 0x0FFFFFFF);

        // write zeroes
        U32 bytes = (U32)clusterBytes;
        U32 pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
        PageAlloc::DMAAlloc::DMABuffer* zbuf = PageAlloc::DMAAlloc::AllocateDMAPages(pages);
        if(!zbuf) return FALSE;
        String::Memset((U8*)zbuf->VirtAddr, 0, bytes);
        WriteCluster(nc, (U8*)zbuf->VirtAddr);
        PageAlloc::DMAAlloc::FreeDMABuffer(zbuf);

        // link from last to nc
        SetNextCluster(last, nc);
        last = nc;
    }

    file->FileSize = (U32)size;
    if((U64)file->CurrentPosition > size) file->CurrentPosition = (U32)size;
    file->Internal_CurrentCluster = last;
    if(file->Internal_StartCluster == 0) file->Internal_StartCluster = last; // safety
    UpdateDirectoryEntry(file);
    return TRUE;
}

BOOL FAT32FileSystem::MKDir(const char* path){
    if(!path || path[0] != '/') return FALSE;

    // Check existence
    File* existing = Open(path);
    if(existing){ Close(existing); return FALSE; }

    // split parent and name
    char parentPath[256]; const char* newName = nullptr;
    size_t len = String::Strlen(path);
    int lastSlash = -1;
    if(len > 0){ for(size_t ui = len; ui > 0; ){ ui--; if(path[ui] == '/') { lastSlash = (int)ui; break; } } }
    if(lastSlash == -1) return FALSE;
    if(lastSlash == 0){ String::Strcpy(parentPath, "/"); newName = path + 1; }
    else { if((size_t)lastSlash >= sizeof(parentPath)) return FALSE; String::Memcpy(parentPath, path, (U64)lastSlash); parentPath[lastSlash] = '\0'; newName = path + lastSlash + 1; }
    if(!newName || String::Strlen(newName) == 0) return FALSE;

    File* parentDir = Open(parentPath);
    if(!parentDir){ return FALSE; }
    if(!parentDir->IsDirectory){ Close(parentDir); return FALSE; }

    U32 parentCl = parentDir->Internal_StartCluster;

    // Allocate a cluster for the new directory
    U32 newCl = AllocateCluster(0);
    if(newCl == 0){ Close(parentDir); return FALSE; }
    // ensure marked EOC
    SetNextCluster(newCl, 0x0FFFFFFF);

    // Initialize cluster contents with '.' and '..'
    U32 clusterBytes = (U32)m_BPB.BytesPerSector * m_BPB.SectorsPerCluster;
    U32 pages = (clusterBytes + PAGE_SIZE - 1) / PAGE_SIZE;
    PageAlloc::DMAAlloc::DMABuffer* buf = PageAlloc::DMAAlloc::AllocateDMAPages(pages);
    if(!buf){ Close(parentDir); return FALSE; }
    String::Memset((U8*)buf->VirtAddr, 0, clusterBytes);

    // Build '.' entry
    U8* s = (U8*)buf->VirtAddr;
    // first entry: '.' in SFN form
    for(int i=0;i<11;i++) s[i] = (i==0) ? (U8)'.' : (U8)' ';
    s[11] = 0x10; // directory attribute
    s[12] = 0; // NTReserved
    // cluster high/low
    s[20] = (U8)((newCl >> 16) & 0xFF);
    s[21] = (U8)((newCl >> 24) & 0xFF);
    s[26] = (U8)(newCl & 0xFF);
    s[27] = (U8)((newCl >> 8) & 0xFF);
    // filesize 0 already zero

    // second entry: '..'
    U8* t = s + 32;
    for(int i=0;i<11;i++) t[i] = (i<2) ? (U8)'.' : (U8)' ';
    t[11] = 0x10; // directory
    t[12] = 0;
    U32 parentCluster = parentDir->Internal_StartCluster;
    if(parentCluster < 2) parentCluster = 0;
    t[20] = (U8)((parentCluster >> 16) & 0xFF);
    t[21] = (U8)((parentCluster >> 24) & 0xFF);
    t[26] = (U8)(parentCluster & 0xFF);
    t[27] = (U8)((parentCluster >> 8) & 0xFF);

    // write cluster
    BOOL ok = WriteCluster(newCl, (U8*)buf->VirtAddr);
    PageAlloc::DMAAlloc::FreeDMABuffer(buf);
    if(!ok){ Close(parentDir); return FALSE; }

    // Create directory entry in parent and fix attribute to directory
    U64 entryLBA = 0; U32 entryOffset = 0;
    if(!CreateDirectoryEntry(parentCl, newName, newCl, 0, &entryLBA, &entryOffset)){
        // free cluster we just created
        FreeClusterChain(newCl);
        Close(parentDir);
        return FALSE;
    }

    // adjust SFN attribute byte to directory (0x10)
    PageAlloc::DMAAlloc::DMABuffer* sbuf = nullptr;
    if(!m_Partition->ReadSectors(entryLBA, 1, &sbuf)){
        Close(parentDir);
        return FALSE;
    }
    U8* sdat = (U8*)sbuf->VirtAddr;
    sdat[entryOffset + 11] = 0x10;
    m_Partition->WriteSectors(entryLBA, 1, sbuf);
    PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);

    Close(parentDir);
    return TRUE;
}

BOOL FAT32FileSystem::RMDir(const char* path){
    if(!m_Partition || !path || path[0] != '/') return FALSE;

    // Split parent and name
    char parentPath[256]; const char* name = nullptr;
    size_t len = String::Strlen(path);
    int lastSlash = -1;
    if(len > 0){ for(size_t ui = len; ui > 0; ){ ui--; if(path[ui] == '/') { lastSlash = (int)ui; break; } } }
    if(lastSlash < 0) return FALSE;
    if(lastSlash == 0){ String::Strcpy(parentPath, "/"); name = path + 1; }
    else { if((size_t)lastSlash >= sizeof(parentPath)) return FALSE; String::Memcpy(parentPath, path, (U64)lastSlash); parentPath[lastSlash] = '\0'; name = path + lastSlash + 1; }
    if(!name || String::Strlen(name) == 0) return FALSE;

    File* parentDir = Open(parentPath);
    if(!parentDir || !parentDir->IsDirectory){ if(parentDir) Close(parentDir); return FALSE; }

    U32 dirStartCluster = parentDir->Internal_StartCluster;

    FAT32_DirectoryEntry de; U64 entryLBA = 0; U32 entryOffset = 0;
    if(!FindFileInDir(name, dirStartCluster, &de, nullptr, 0, &entryLBA, &entryOffset)){
        Close(parentDir); return FALSE;
    }

    if(!(de.Attributes & 0x10)) { Close(parentDir); return FALSE; }

    // Ensure directory is empty (only '.' and '..')
    struct Ctx { int count; } ctx; ctx.count = 0;
    auto cb = [](const char* fname, FAT32_DirectoryEntry* de2, void* vctx, U64 lba, U32 off)->BOOL{
        (void)lba; (void)off; Ctx* c = (Ctx*)vctx;
        if(String::Strcmp(fname, ".") == 0) return TRUE;
        if(String::Strcmp(fname, "..") == 0) return TRUE;
        // found a real entry: mark and stop
        c->count = 1;
        return FALSE;
    };

    if(!ReadDirectory(((U32)de.ClusterHigh << 16) | (U32)de.ClusterLow, cb, &ctx)){
        Close(parentDir); return FALSE;
    }
    if(ctx.count > 0){ Close(parentDir); return FALSE; }

    // free clusters of the directory
    U32 startCluster = ((U32)de.ClusterHigh << 16) | (U32)de.ClusterLow;
    if(startCluster >= 2) FreeClusterChain(startCluster);

    // mark SFN deleted
    PageAlloc::DMAAlloc::DMABuffer* sbuf = nullptr;
    if(!m_Partition->ReadSectors(entryLBA, 1, &sbuf)){ Close(parentDir); return FALSE; }
    U8* sdata = (U8*)sbuf->VirtAddr;
    sdata[entryOffset + 0] = 0xE5;
    BOOL wrote = m_Partition->WriteSectors(entryLBA, 1, sbuf);
    PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);
    if(!wrote){ Close(parentDir); return FALSE; }

    // delete preceding LFN entries similar to Delete()
    U32 sectorsPerCluster = m_BPB.SectorsPerCluster;

    // Build list of sector LBAs that make up this directory
    U32 clCount = 0;
    {
        U32 c = dirStartCluster; U32 guard2 = 0;
        while(c >= 2 && c < 0x0FFFFFF8){ clCount++; c = GetNextCluster(c); if(++guard2 > m_TotalClusters + 2) break; }
    }
    U64 totalSectors = (U64)clCount * (U64)sectorsPerCluster;
    if(totalSectors == 0){ Close(parentDir); return TRUE; }

    U64* sectorLBAs = (U64*)Kmalloc::Alloc((U32)(totalSectors * sizeof(U64)));
    if(!sectorLBAs){ Close(parentDir); return TRUE; }

    {
        U32 c = dirStartCluster; U64 idx = 0; U32 guard3 = 0;
        while(c >= 2 && c < 0x0FFFFFF8 && idx < totalSectors){ sectorLBAs[idx++] = ClusterToLBA(c); c = GetNextCluster(c); if(++guard3 > m_TotalClusters + 2) break; }
    }

    long long sfnIdx = -1;
    for(U64 i=0;i<totalSectors;i++){
        if(sectorLBAs[i] == entryLBA){ sfnIdx = (long long)i; break; }
    }

    if(sfnIdx >= 0){
        // scan backwards and delete LFN entries
        PageAlloc::DMAAlloc::DMABuffer* curBuf = nullptr;
        for(long long secIdx = sfnIdx; secIdx >= 0; --secIdx){
            if(!m_Partition->ReadSectors(sectorLBAs[secIdx], 1, &curBuf)) break;
            U8* data = (U8*)curBuf->VirtAddr;
            U32 bps = (U32)m_BPB.BytesPerSector;
            BOOL dirty = FALSE;
            for(U32 offs = bps - 32; ; offs -= 32){
                U8 first = data[offs + 0]; U8 attr = data[offs + 11];
                if(first == 0x00) break; // end
                if(attr != 0x0F) break; // stop
                data[offs + 0] = 0xE5; dirty = TRUE;
                if(offs < 32) break;
            }
            if(dirty) m_Partition->WriteSectors(sectorLBAs[secIdx], 1, curBuf);
            if(curBuf) PageAlloc::DMAAlloc::FreeDMABuffer(curBuf);
        }
    }

    Kmalloc::Free(sectorLBAs);
    Close(parentDir);
    return TRUE;
}

BOOL FAT32FileSystem::Flush(File* file){
    if(!file) return FALSE;
    // Flush directory entry metadata to disk
    return UpdateDirectoryEntry(file);
}

// Append: move to EOF and write the buffer. Returns TRUE if all bytes written.
BOOL FAT32FileSystem::Append(File* file, U8* buffer, U32 size){
    if(!file || !buffer || size == 0) return FALSE;
    if(file->IsDirectory) return FALSE;

    // seek to end
    if(!Seek(file, file->FileSize)) return FALSE;

    U32 written = Write(file, buffer, size);
    return (written == size) ? TRUE : FALSE;
}

INTN FAT32FileSystem::ReadDir(File* dirFile, void* buffer, U32 bufferSize){
    if(!dirFile || !buffer) return -1;
    if(!dirFile->IsDirectory) return -1;
    if(bufferSize == 0) return 0;

    char* out = (char*)buffer;
    U32 remain = bufferSize;

    // Treat CurrentPosition as number of entries already returned.
    U64 skip = dirFile->CurrentPosition;
    U64 seen = 0;
    bool overflow = false;

    struct Ctx { char* out; U32 remain; U64 skip; U64 *seen; bool *overflow; } ctx;
    ctx.out = out; ctx.remain = remain; ctx.skip = skip; ctx.seen = &seen; ctx.overflow = &overflow;

    // callback invoked for each directory entry name
    auto cb = [](const char* name, FAT32_DirectoryEntry* de, void* vctx, U64 lba, U32 off)->BOOL{
        (void)de; (void)lba; (void)off;
        Ctx* c = (Ctx*)vctx;
        if(!name){ return TRUE; }
        // increment seen count for every valid entry
        U64 idx = (*c->seen)++;
        if(idx < c->skip) return TRUE; // skip already-read entries
        U32 needed = (U32)(String::Strlen(name) + 1);
        if(needed > c->remain){ (*c->seen)--; *(c->overflow) = true; return FALSE; }
        String::Memcpy((U8*)c->out, (const U8*)name, needed);
        c->out += needed;
        c->remain -= needed;
        return TRUE; // continue
    };

    BOOL ok = ReadDirectory(dirFile->Internal_StartCluster, cb, &ctx);
    // Update out/remaining from ctx
    out = ctx.out; remain = ctx.remain;

    // Update file CurrentPosition to number of entries seen
    dirFile->CurrentPosition = (U64)seen;

    if(!ok && !overflow) return -1;
    U32 used = bufferSize - remain;
    return (INTN)used;
}