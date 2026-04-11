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


U64 FAT32FileSystem::Lookup(const char* path) {
    if (!path || path[0] == '\0') return 0;

    FAT32_VNode* node = new FAT32_VNode();
    node->RefCount = 1;

    // Kalau VFS nyari Root Directory ("/")
    if (path[0] == '/' && path[1] == '\0') {
        node->StartCluster = m_BPB.RootDirCluster;
        node->FileSize = 0; 
        node->IsDirectory = TRUE;
        node->EntryLBA = 0;
        node->EntryOffset = 0;
        return (U64)(UPTR)node;
    }

    U32 currCluster = m_BPB.RootDirCluster;
    FAT32_DirectoryEntry entry;
    U64 entryLBA = 0;
    U32 entryOffset = 0;

    // Bikin copy path biar bisa di-tokenize
    char tempPath[512];
    String::Strcpy(tempPath, path);
    char* token = tempPath;
    
    // Skip leading slash
    if (*token == '/') token++; 

    char* nextToken = nullptr;
    BOOL found = FALSE;

    // Manual tokenization pake logika string standar
    while (*token) {
        nextToken = token;
        while (*nextToken && *nextToken != '/') nextToken++;
        
        if (*nextToken == '/') {
            *nextToken = '\0';
            nextToken++;
        }

        // Cari file/folder ini di dalem cluster current
        if (!FindFileInDir(token, currCluster, &entry, nullptr, 0, &entryLBA, &entryOffset)) {
            delete node;
            return 0; // Gak ketemu!
        }

        currCluster = (entry.ClusterHigh << 16) | entry.ClusterLow;
        found = TRUE;

        token = nextToken;
        while (*token == '/') token++; // Skip multiple slashes kayak "//"
    }

    if (!found) {
        delete node;
        return 0;
    }

    // Ketemu! Masukin metadata ke RAM.
    node->StartCluster = currCluster;
    node->FileSize = entry.FileSize;
    node->IsDirectory = (entry.Attributes & 0x10) != 0;
    node->EntryLBA = entryLBA;
    node->EntryOffset = entryOffset;

    return (U64)(UPTR)node; // Lempar pointernya ke VFS
}

BOOL FAT32FileSystem::PopulateInode(U64 InodeID, ::Inode* vfsNode) {
    if (InodeID == 0 || !vfsNode) return FALSE;

    // Tarik balik metadata dari RAM
    FAT32_VNode* node = (FAT32_VNode*)(UPTR)InodeID;

    // Isi struct Inode punya VFS
    vfsNode->Type = node->IsDirectory ? FT_DIR : FT_NORMAL;
    vfsNode->FileSize = node->FileSize;
    vfsNode->FSOwner = this;
    vfsNode->InodeID = InodeID; // Tetep simpen alamat pointernya

    return TRUE;
}

U64 FAT32FileSystem::CreateNode(const char* path, U32 Flags) {
    if (!path || path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) return 0;

    char tempPath[512];
    String::Strcpy(tempPath, path);

    // Cari posisi slash terakhir buat misahin nama file & parent dir
    char* lastSlash = nullptr;
    char* ptr = tempPath;
    while (*ptr) {
        if (*ptr == '/') lastSlash = ptr;
        ptr++;
    }

    char* fileName = nullptr;
    U32 parentCluster = m_BPB.RootDirCluster;

    if (lastSlash) {
        *lastSlash = '\0'; // Potong string di slash terakhir
        fileName = lastSlash + 1;
        char* parentPath = tempPath;
        
        if (parentPath[0] != '\0') {
            // Rekursif manja: Lookup parent directory-nya dulu
            U32 parentID = Lookup(parentPath);
            if (parentID == 0) return 0; // Parent directory gak ada!
            
            FAT32_VNode* pNode = (FAT32_VNode*)(UPTR)parentID;
            if (!pNode->IsDirectory) {
                delete pNode;
                return 0; // Wah, parent-nya ternyata bukan folder!
            }
            parentCluster = pNode->StartCluster;
            delete pNode; // Bersihin temporary node
        }
    } else {
        fileName = tempPath;
    }

    if (!fileName || fileName[0] == '\0') return 0;

    // Panggil helper dewa lu buat nulis LFN + SFN ke hardisk
    U64 entryLBA = 0;
    U32 entryOffset = 0;
    
    // File baru ukurannya 0, StartCluster 0 (nanti pas Write baru dialokasiin cluster)
    if (!CreateDirectoryEntry(parentCluster, fileName, 0, 0, &entryLBA, &entryOffset)) {
        return 0; // Disk penuh / error nulis
    }

    // File berhasil dibikin, sekarang return InodeID-nya ke VFS
    FAT32_VNode* node = new FAT32_VNode();
    node->StartCluster = 0;
    node->FileSize = 0;
    node->IsDirectory = FALSE;
    node->EntryLBA = entryLBA;
    node->EntryOffset = entryOffset;
    node->RefCount = 1;

    return (UPTR)node;
}

void FAT32FileSystem::Close(File* file){
    if(!file || !file->Node) return;
    
    // Tarik metadata dari InodeID
    FAT32_VNode* node = (FAT32_VNode*)(UPTR)file->Node->InodeID;
    if(node){
        delete node; // Bersihin memori VNode-nya
    }
    // GAK ADA 'delete file;' di sini!
}

U32 FAT32FileSystem::Read(File* file, U8* buffer, U32 size){
    if(!file || !buffer || size == 0 || !file->Node) return 0;
    
    FAT32_VNode* node = (FAT32_VNode*)(UPTR)file->Node->InodeID;
    if(!node) return 0;

    // Gak logis nge-read direktori pake fungsi ini (biasanya pake ReadDir)
    if(node->IsDirectory) return 0; 

    if(file->CurrentPosition >= node->FileSize) return 0;

    U64 bytesPerCluster = (U64)m_BPB.BytesPerSector * (U64)m_BPB.SectorsPerCluster;
    U64 remainingFile = node->FileSize - file->CurrentPosition;
    U32 toRead = (U32)((size > remainingFile) ? remainingFile : size);
    U32 totalRead = 0;

    U32 clusterIndex = file->CurrentPosition / bytesPerCluster;
    U32 offsetInCluster = file->CurrentPosition % bytesPerCluster;

    // Tarik StartCluster dari node!
    U32 cluster = node->StartCluster;
    if (cluster == 0) return 0; // File kosong

    // Walk to the clusterIndex
    for(U32 i = 0; i < clusterIndex; i++){
        cluster = GetNextCluster(cluster);
        if(cluster >= 0x0FFFFFF8) return totalRead; // end of chain
    }

    while(toRead > 0){
        PageAlloc::DMAAlloc::DMABuffer* buf = nullptr;
        U64 lba = ClusterToLBA(cluster);
        if(!m_Partition->ReadSectors(lba, m_BPB.SectorsPerCluster, &buf)){
            break;
        }

        U8* clusterData = (U8*)buf->VirtAddr;
        U32 avail = (U32)(bytesPerCluster - offsetInCluster);
        U32 want = (toRead < avail) ? toRead : avail;
        String::Memcpy(buffer + totalRead, clusterData + offsetInCluster, want);
        PageAlloc::DMAAlloc::FreeDMABuffer(buf);

        totalRead += want;
        toRead -= want;
        offsetInCluster = 0; 

        if(toRead == 0) break;

        cluster = GetNextCluster(cluster);
        if(cluster >= 0x0FFFFFF8) break; 
    }

    file->CurrentPosition += totalRead;
    return totalRead;
}

U32 FAT32FileSystem::Write(File* file, U8* buffer, U32 size){
    if(!file || !buffer || size == 0 || !file->Node) return 0;

    FAT32_VNode* node = (FAT32_VNode*)(UPTR)file->Node->InodeID;
    if(!node) return 0;

    // Root dir (EntryLBA == 0) atau folder gak boleh ditimpa sembarangan
    if(node->EntryLBA == 0 || node->IsDirectory) return 0;

    U64 clusterBytes = (U64)m_BPB.BytesPerSector * (U64)m_BPB.SectorsPerCluster;
    if(clusterBytes == 0) return 0;

    U64 startPos = (U64)file->CurrentPosition; 
    U32 totalWritten = 0;

    // Alokasi cluster pertama kalau file ini masih 0 bytes!
    U32 cluster = node->StartCluster;
    if(cluster == 0){
        U32 nc = AllocateCluster(0);
        if(nc == 0) return 0;
        node->StartCluster = nc; // Simpen di node RAM
        cluster = nc;
    }

    U64 clusterIndex = startPos / clusterBytes;
    U32 cur = cluster;
    U32 prev = 0;
    
    for(U64 i = 0; i < clusterIndex; ++i){
        prev = cur;
        U32 next = GetNextCluster(cur);
        if(next >= 0x0FFFFFF8){
            U32 newc = AllocateCluster(cur);
            if(newc == 0) return totalWritten;
            cur = newc;
        } else {
            cur = next;
        }
    }
    cluster = cur;

    U32 offsetInCluster = (U32)(startPos % clusterBytes);
    U32 remaining = size;
    U8* srcPtr = buffer;

    while(remaining > 0){
        if(cluster == 0){
            U32 newc = AllocateCluster(prev);
            if(newc == 0) break;
            cluster = newc;
        }

        U32 can = (U32)(clusterBytes - offsetInCluster);
        U32 want = (remaining < can) ? remaining : can;

        if(offsetInCluster == 0 && want == clusterBytes){
            if(!WriteCluster(cluster, srcPtr)) break;
        } else {
            U32 cbytes = (U32)clusterBytes;
            U8* tmp = (U8*)Kmalloc::Alloc(cbytes);
            if(!tmp) break;
            
            if(!ReadCluster(cluster, tmp)){
                for(U32 z=0; z<cbytes; ++z) tmp[z] = 0;
            }
            String::Memcpy(tmp + offsetInCluster, srcPtr, want);

            if(!WriteCluster(cluster, tmp)){
                Kmalloc::Free(tmp);
                break;
            }
            Kmalloc::Free(tmp);
        }

        totalWritten += want;
        remaining -= want;
        srcPtr += want;
        offsetInCluster = 0; 

        prev = cluster;
        U32 nextc = GetNextCluster(cluster);
        if(remaining > 0){
            if(nextc >= 0x0FFFFFF8){
                U32 newc = AllocateCluster(cluster);
                if(newc == 0) break;
                cluster = newc;
            } else {
                cluster = nextc;
            }
        }
    }

    // UPDATE METADATA (PENTING!)
    U64 newPos = startPos + (U64)totalWritten;
    if(newPos > node->FileSize){
        node->FileSize = newPos;              // Update Node FAT32
        file->Node->FileSize = newPos;        // Update Inode VFS biar sinkron!
    }
    file->CurrentPosition = (U32)newPos;

    // Simpen ke Hardisk!
    UpdateDirectoryEntry(file);

    return totalWritten;
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

    // 2) Ganti Open() dengan Lookup() untuk dapetin parent directory
    U32 parentID = Lookup(parentPath);
    if(parentID == 0) return FALSE;

    FAT32_VNode* parentNode = (FAT32_VNode*)(UPTR)parentID;
    if(!parentNode->IsDirectory){ 
        delete parentNode; 
        return FALSE; 
    }

    U32 dirStartCluster = parentNode->StartCluster;

    // 3) Find the file entry in the directory
    FAT32_DirectoryEntry de;
    U64 entryLBA = 0; U32 entryOffset = 0;
    if(!FindFileInDir(name, dirStartCluster, &de, nullptr, 0, &entryLBA, &entryOffset)){
        delete parentNode;
        return FALSE; // not found
    }

    // Don't delete directories in this function
    if(de.Attributes & 0x10){
        delete parentNode;
        return FALSE;
    }

    // 4) Free the cluster chain for the file
    U32 startCluster = ((U32)de.ClusterHigh << 16) | (U32)de.ClusterLow;
    if(startCluster >= 2) {
        FreeClusterChain(startCluster);
    }

    // 5 & 6) Hapus SFN & LFN menggunakan Helper
    RemoveDirectoryEntryAndLFNs(dirStartCluster, entryLBA, entryOffset);

    // Jangan lupa bersihin node-nya
    delete parentNode;
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

    // Lookup old parent
    U32 oldParentID = Lookup(oldParent);
    if(oldParentID == 0) return FALSE;
    FAT32_VNode* oldParentNode = (FAT32_VNode*)(UPTR)oldParentID;
    if(!oldParentNode->IsDirectory){ delete oldParentNode; return FALSE; }

    FAT32_DirectoryEntry oldDe; U64 oldLBA = 0; U32 oldOff = 0;
    if(!FindFileInDir(oldName, oldParentNode->StartCluster, &oldDe, nullptr, 0, &oldLBA, &oldOff)){
        delete oldParentNode; return FALSE;
    }
    // For now, do not rename directories
    if(oldDe.Attributes & 0x10){ delete oldParentNode; return FALSE; }

    // Lookup new parent
    U32 newParentID = Lookup(newParent);
    if(newParentID == 0){ delete oldParentNode; return FALSE; }
    FAT32_VNode* newParentNode = (FAT32_VNode*)(UPTR)newParentID;
    if(!newParentNode->IsDirectory){ delete oldParentNode; delete newParentNode; return FALSE; }

    FAT32_DirectoryEntry tmpDe; U64 tmpLBA; U32 tmpOff;
    if(FindFileInDir(newName, newParentNode->StartCluster, &tmpDe, nullptr, 0, &tmpLBA, &tmpOff)){
        // name exists di target, batalin rename
        delete oldParentNode; delete newParentNode; return FALSE;
    }

    // Create a new entry at destination with same start cluster and size
    U32 startCluster = ((U32)oldDe.ClusterHigh << 16) | (U32)oldDe.ClusterLow;
    if(!CreateDirectoryEntry(newParentNode->StartCluster, newName, startCluster, oldDe.FileSize)){
        delete oldParentNode; delete newParentNode; return FALSE;
    }

    // Delete old entry (SFN + preceding LFN entries) menggunakan Helper
    RemoveDirectoryEntryAndLFNs(oldParentNode->StartCluster, oldLBA, oldOff);

    delete oldParentNode;
    delete newParentNode;
    return TRUE;
}

BOOL FAT32FileSystem::Seek(File* file, U64 position, U32 Origin){
    // Validasi VFS standar
    if(!file || !file->Node) return FALSE;

    FAT32_VNode* node = (FAT32_VNode*)(UPTR)file->Node->InodeID;
    if(!node) return FALSE;

    // Directories: do not support seeking for now
    if(node->IsDirectory) return FALSE;

    // Clamp position within [0, FileSize] dari VNode
    if(position > node->FileSize) position = node->FileSize;

    // Fast path: if seeking to current, nothing to do
    if((U64)file->CurrentPosition == position) return TRUE;

    // KARENA di Read/Write kita udah nge-walk cluster chain dinamis 
    // dari node->StartCluster, kita GAK PERLU lagi nyimpen CurrentCluster di File.
    // Seek cukup update CurrentPosition aja! O(1) performance boi!
    file->CurrentPosition = (U32)position;
    
    return TRUE;
}

BOOL FAT32FileSystem::Truncate(File* file, U64 size){
    if(!file || !file->Node) return FALSE;

    FAT32_VNode* node = (FAT32_VNode*)(UPTR)file->Node->InodeID;
    if(!node) return FALSE;

    if(node->IsDirectory) return FALSE;

    U64 clusterBytes = (U64)m_BPB.BytesPerSector * (U64)m_BPB.SectorsPerCluster;
    if(clusterBytes == 0) return FALSE;

    // Ambil old size dari VNode
    U64 oldSize = node->FileSize;
    if(size == oldSize) return TRUE;

    // Shrink
    if(size < oldSize){
        U32 keepClusters = (size == 0) ? 0 : (U32)((size + clusterBytes - 1) / clusterBytes);

        U32 start = node->StartCluster;
        if(keepClusters == 0){
            if(start >= 2){
                FreeClusterChain(start);
            }
            node->StartCluster = 0;
            
            // Update metadata VNode dan VFS
            node->FileSize = (U32)size;
            file->Node->FileSize = (U32)size;
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

        // Update metadata VNode dan VFS
        node->FileSize = (U32)size;
        file->Node->FileSize = (U32)size;
        if((U64)file->CurrentPosition > size) file->CurrentPosition = (U32)size;
        
        UpdateDirectoryEntry(file);
        return TRUE;
    }

    // Extend: allocate clusters and zero them
    U32 oldClusters = (oldSize == 0) ? 0 : (U32)((oldSize + clusterBytes - 1) / clusterBytes);
    U32 newClusters = (size == 0) ? 0 : (U32)((size + clusterBytes - 1) / clusterBytes);
    
    if(newClusters <= oldClusters){
        // file size grows within existing cluster
        node->FileSize = (U32)size;
        file->Node->FileSize = (U32)size;
        UpdateDirectoryEntry(file);
        return TRUE;
    }

    U32 need = newClusters - oldClusters;
    U32 last = node->StartCluster;

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

    // Update metadata VNode dan VFS
    node->FileSize = (U32)size;
    file->Node->FileSize = (U32)size;
    if(node->StartCluster == 0) node->StartCluster = last; // safety
    
    UpdateDirectoryEntry(file);
    return TRUE;
}

BOOL FAT32FileSystem::MKDir(const char* path){
    if(!path || path[0] != '/') return FALSE;

    // Check existence pake Lookup (Bukan Open)
    U32 existingID = Lookup(path);
    if(existingID != 0){ 
        // Kalau ketemu (file/dir udah ada), bersihin node-nya trus gagal
        delete (FAT32_VNode*)(UPTR)existingID; 
        return FALSE; 
    }

    // split parent and name
    char parentPath[256]; const char* newName = nullptr;
    size_t len = String::Strlen(path);
    int lastSlash = -1;
    if(len > 0){ for(size_t ui = len; ui > 0; ){ ui--; if(path[ui] == '/') { lastSlash = (int)ui; break; } } }
    if(lastSlash == -1) return FALSE;
    if(lastSlash == 0){ String::Strcpy(parentPath, "/"); newName = path + 1; }
    else { if((size_t)lastSlash >= sizeof(parentPath)) return FALSE; String::Memcpy(parentPath, path, (U64)lastSlash); parentPath[lastSlash] = '\0'; newName = path + lastSlash + 1; }
    if(!newName || String::Strlen(newName) == 0) return FALSE;

    // Buka Parent pake Lookup
    U32 parentID = Lookup(parentPath);
    if(parentID == 0) return FALSE;
    
    FAT32_VNode* parentNode = (FAT32_VNode*)(UPTR)parentID;
    if(!parentNode->IsDirectory){ 
        delete parentNode; 
        return FALSE; 
    }

    U32 parentCl = parentNode->StartCluster;

    // Allocate a cluster for the new directory
    U32 newCl = AllocateCluster(0);
    if(newCl == 0){ delete parentNode; return FALSE; }
    // ensure marked EOC
    SetNextCluster(newCl, 0x0FFFFFFF);

    // Initialize cluster contents with '.' and '..'
    U32 clusterBytes = (U32)m_BPB.BytesPerSector * m_BPB.SectorsPerCluster;
    U32 pages = (clusterBytes + PAGE_SIZE - 1) / PAGE_SIZE;
    PageAlloc::DMAAlloc::DMABuffer* buf = PageAlloc::DMAAlloc::AllocateDMAPages(pages);
    if(!buf){ delete parentNode; return FALSE; }
    String::Memset((U8*)buf->VirtAddr, 0, clusterBytes);

    // Build '.' entry
    U8* s = (U8*)buf->VirtAddr;
    for(int i=0;i<11;i++) s[i] = (i==0) ? (U8)'.' : (U8)' ';
    s[11] = 0x10; // directory attribute
    s[12] = 0; // NTReserved
    s[20] = (U8)((newCl >> 16) & 0xFF);
    s[21] = (U8)((newCl >> 24) & 0xFF);
    s[26] = (U8)(newCl & 0xFF);
    s[27] = (U8)((newCl >> 8) & 0xFF);

    // Build '..' entry
    U8* t = s + 32;
    for(int i=0;i<11;i++) t[i] = (i<2) ? (U8)'.' : (U8)' ';
    t[11] = 0x10; // directory
    t[12] = 0;
    
    // Parent root biasanya dinilai 0 di struktur FAT32
    U32 parentClusterForDotDot = parentCl;
    if(parentClusterForDotDot < 2) parentClusterForDotDot = 0;
    
    t[20] = (U8)((parentClusterForDotDot >> 16) & 0xFF);
    t[21] = (U8)((parentClusterForDotDot >> 24) & 0xFF);
    t[26] = (U8)(parentClusterForDotDot & 0xFF);
    t[27] = (U8)((parentClusterForDotDot >> 8) & 0xFF);

    // write cluster
    BOOL ok = WriteCluster(newCl, (U8*)buf->VirtAddr);
    PageAlloc::DMAAlloc::FreeDMABuffer(buf);
    if(!ok){ delete parentNode; return FALSE; }

    // Create directory entry in parent and fix attribute to directory
    U64 entryLBA = 0; U32 entryOffset = 0;
    if(!CreateDirectoryEntry(parentCl, newName, newCl, 0, &entryLBA, &entryOffset)){
        FreeClusterChain(newCl);
        delete parentNode;
        return FALSE;
    }

    // adjust SFN attribute byte to directory (0x10)
    PageAlloc::DMAAlloc::DMABuffer* sbuf = nullptr;
    if(!m_Partition->ReadSectors(entryLBA, 1, &sbuf)){
        delete parentNode;
        return FALSE;
    }
    U8* sdat = (U8*)sbuf->VirtAddr;
    sdat[entryOffset + 11] = 0x10;
    m_Partition->WriteSectors(entryLBA, 1, sbuf);
    PageAlloc::DMAAlloc::FreeDMABuffer(sbuf);

    delete parentNode;
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

    // Lookup Parent Directory
    U32 parentID = Lookup(parentPath);
    if(parentID == 0) return FALSE;
    
    FAT32_VNode* parentNode = (FAT32_VNode*)(UPTR)parentID;
    if(!parentNode->IsDirectory){ 
        delete parentNode; 
        return FALSE; 
    }

    U32 dirStartCluster = parentNode->StartCluster;

    FAT32_DirectoryEntry de; U64 entryLBA = 0; U32 entryOffset = 0;
    if(!FindFileInDir(name, dirStartCluster, &de, nullptr, 0, &entryLBA, &entryOffset)){
        delete parentNode; 
        return FALSE;
    }

    // Pastikan ini benar-benar directory
    if(!(de.Attributes & 0x10)) { 
        delete parentNode; 
        return FALSE; 
    }

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

    U32 targetCluster = ((U32)de.ClusterHigh << 16) | (U32)de.ClusterLow;
    if(!ReadDirectory(targetCluster, cb, &ctx)){
        delete parentNode; 
        return FALSE;
    }
    if(ctx.count > 0){ 
        delete parentNode; // Gagal kalau folder gak kosong
        return FALSE; 
    }

    // Free clusters of the directory itself
    if(targetCluster >= 2) FreeClusterChain(targetCluster);

    // Hapus SFN dan preceding LFN pakai helper yang udah dibikin!
    RemoveDirectoryEntryAndLFNs(dirStartCluster, entryLBA, entryOffset);

    // Bersih-bersih
    delete parentNode;
    return TRUE;
}

BOOL FAT32FileSystem::Flush(File* file){
    if(!file) return FALSE;
    // Flush directory entry metadata to disk
    return UpdateDirectoryEntry(file);
}

BOOL FAT32FileSystem::Append(File* file, U8* buffer, U32 size){
    if(!file || !file->Node || !buffer || size == 0) return FALSE;
    
    FAT32_VNode* node = (FAT32_VNode*)(UPTR)file->Node->InodeID;
    if(!node || node->IsDirectory) return FALSE;

    // 1. Langsung paksa CurrentPosition ke ujung file (Absolute)
    // Kita panggil Seek pake posisi = FileSize, Origin = SEEK_SET
    if(!Seek(file, node->FileSize, 0)) return FALSE; 

    // 2. Tulis datanya
    // Fungsi Write lu harusnya otomatis meng-handle alokasi cluster baru
    // dan update node->FileSize serta file->Node->FileSize.
    U32 written = Write(file, buffer, size);
    
    return (written == size);
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

    BOOL ok = ReadDirectory(dirFile->Node->Internal_StartCluster, cb, &ctx);
    // Update out/remaining from ctx
    out = ctx.out; remain = ctx.remain;

    // Update file CurrentPosition to number of entries seen
    dirFile->CurrentPosition = (U64)seen;

    if(!ok && !overflow) return -1;
    U32 used = bufferSize - remain;
    return (INTN)used;
}

BOOL FAT32FileSystem::Unmount(){
    m_Partition = nullptr;

    Printk::Write(Printk::Level::LOG_INFO, "FAT32: Unmounted successfully.\n");
    return TRUE;
}

BOOL FAT32FileSystem::Stat(const char *path, FileInfo *bufferout){
    if(!path || !bufferout || !m_Partition) return FALSE;

    // 1. Cari Inode-nya lewat Lookup
    // Lookup bakal balikin pointer FAT32_VNode yang di-cast ke U32
    U32 inodeID = Lookup(path);
    if(inodeID == 0) return FALSE;

    FAT32_VNode* node = (FAT32_VNode*)(UPTR)inodeID;

    // 2. Salin metadata dari VNode ke bufferout
    bufferout->Size = node->FileSize;
    bufferout->InodeID = (U64)inodeID;
    bufferout->IsDirectory = node->IsDirectory;
    bufferout->Type = node->IsDirectory ? FT_DIR : FT_NORMAL;
    
    // FAT32 nggak punya permission ala UNIX, kasih default aja
    bufferout->Mode = node->IsDirectory ? 0755 : 0644;
    
    // Kalau lu mau niat, bisa ambil timestamp dari Directory Entry, 
    // tapi buat sekarang 0 juga aman.
    bufferout->CreationTime = 0;

    // 3. PENTING: Karena Lookup mengalokasikan FAT32_VNode baru, 
    // kita harus delete setelah selesai dipake di sini.
    delete node;

    return TRUE;
}

I64 FAT32FileSystem::ReadLink(const char* path, char* outLink, U64 outLinkSize)
{
    // FAT32 doesn't support symlinks
    return -1;
}
