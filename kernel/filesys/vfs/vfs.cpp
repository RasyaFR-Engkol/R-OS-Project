#include "rosval.h"
#define PRINTK_MODULE_NAME "VFS"
#include "vfs.hpp"
#include <string.hpp>
#include <logging.hpp>
#include "../../dev/devicemanager.hpp"
#include <rwlock_simple.hpp>
#include "../../mm/shm/shm.hpp"
#include "../pipefs/pipe.hpp"

#define MAX_FS_DRIVERS 10
#define MAX_MOUNT_POINTS 20

namespace {
    #define MAX_VFS_CACHED_INODES 128
    ::Inode* g_InodeCache[MAX_VFS_CACHED_INODES] = {nullptr};

    ::Inode* GetCachedInode(FileSystem* fs, U64 InodeNum) {
        for (int i = 0; i < MAX_VFS_CACHED_INODES; i++) {
            // Harus cek FS-nya juga! Siapa tau Inode 5 di EXT2 beda sama Inode 5 di FAT32
            if (g_InodeCache[i] != nullptr && 
                g_InodeCache[i]->FSOwner == fs && 
                g_InodeCache[i]->InodeID == InodeNum) {
                return g_InodeCache[i];
            }
        }
        return nullptr;
    }

    void AddCachedInode(::Inode* node) {
        for (int i = 0; i < MAX_VFS_CACHED_INODES; i++) {
            if (g_InodeCache[i] == nullptr) {
                g_InodeCache[i] = node;
                return;
            }
        }
        Printk::Write(Printk::Level::LOG_WARNING, "VFS: Inode Cache Full!\n");
    }

    VOID RemoveCachedInode(FileSystem *Fs, U64 InodeNum){
        for(int i = 0; i < MAX_VFS_CACHED_INODES; i++){
            if(g_InodeCache[i] != nullptr && 
               g_InodeCache[i]->FSOwner == Fs && 
               g_InodeCache[i]->InodeID == InodeNum){
                g_InodeCache[i] = nullptr;
                return;
            }
        }
    }
}

VOID CanonicalizePath(const char* cwd, const char* input, char* output) {
    char temp[256];
    
    // 1. Handle Absolute vs Relative
    if (input[0] == '/') {
        // Absolute path
        String::Strcpy(temp, input);
    } else {
        // Relative path: Gabung CWD + Input
        String::Strcpy(temp, cwd);
        int len = String::Strlen(temp);
        if (len > 0 && temp[len-1] != '/') String::Strcat(temp, "/");
        String::Strcat(temp, input);
    }

    // 2. Tokenize dan Rebuild
    // Kita pakai stack sederhana untuk handle ".."
    char* tokens[32]; // Max depth 32
    int top = 0;
    
    char work_buf[256];
    String::Strcpy(work_buf, temp);

    UNUSED__ char* context = nullptr;
    // Asumsi lu punya String::Strtok atau sejenisnya. 
    // Kalau pakai Tokenize lu yg di userland tadi, sesuaikan logicnya.
    // Disini saya pakai logic manual parsing "/" biar aman.
    
    int len = String::Strlen(work_buf);
    char* start = work_buf;
    
    for (int i = 0; i <= len; i++) {
        if (work_buf[i] == '/' || work_buf[i] == '\0') {
            work_buf[i] = '\0';
            if (String::Strlen(start) > 0) {
                if (String::Strcmp(start, ".") == 0) {
                    // Ignore "."
                } else if (String::Strcmp(start, "..") == 0) {
                    // Pop stack
                    if (top > 0) top--;
                } else {
                    // Push stack
                    tokens[top++] = start;
                }
            }
            start = &work_buf[i+1];
        }
    }

    // 3. Reconstruct Output
    if (top == 0) {
        String::Strcpy(output, "/");
        return;
    }

    output[0] = '\0';
    for (int i = 0; i < top; i++) {
        String::Strcat(output, "/");
        String::Strcat(output, tokens[i]);
    }
}

namespace VFSManager{
    struct FSDriver{
        char name[32];
        FSDriverFactory factory;
    };
    static FSDriver g_Drivers[MAX_FS_DRIVERS];
    static U32 g_DriverCount = 0;

    struct MountPoint{
        char path[128];
        FileSystem* fs;
    };
    static MountPoint g_MountPoints[MAX_MOUNT_POINTS];
    static U32 g_MountPointCount = 0;
    static RwLock g_VFSLock; // Lock global untuk operasi VFS (Mount, Resolve, dll)

    VOID RegisterFileSystem(const char *fsName, FSDriverFactory factory){
        if(g_DriverCount >= MAX_FS_DRIVERS){
            Printk::Write(Printk::Level::LOG_ERR, "VFS: Cannot register FS driver %s, max drivers reached\n", fsName);
            return;
        }
        String::Strcpy(g_Drivers[g_DriverCount].name, fsName);
        g_Drivers[g_DriverCount].factory = factory;
        g_DriverCount++;
        Printk::Write(Printk::Level::LOG_INFO, "VFS: Registered FS driver %s\n", fsName);
    }

    FileSystem *InstantiateDriver(const char *name){
        for(U32 i = 0; i < g_DriverCount; i++){
            if(String::Strcmp(g_Drivers[i].name, name) == 0){
                return g_Drivers[i].factory();
            }
        }
        return nullptr;
    }

    // Internal helper: find mount point index by path (exact match)
    static int FindMountIndex(const char* path){
        for(U32 i=0;i<g_MountPointCount;i++){
            if(String::Strcmp(g_MountPoints[i].path, path) == 0) return (int)i;
        }
        return -1;
    }

    U32 GetMountPointCount(){
        return g_MountPointCount;
    }

    CONSTANT CHAR8* GetMountPointPath(U32 index){
        if(index >= g_MountPointCount) return nullptr;
        return (CONSTANT CHAR8*)g_MountPoints[index].path;
    }

    BOOL Mount(const char *path, Partition *Part){
        if(!path || !Part) return FALSE;
        if(g_MountPointCount >= MAX_MOUNT_POINTS) return FALSE;
        if(path[0] != '/') return FALSE; // require absolute
        if(FindMountIndex(path) >= 0){
            Printk::Write(Printk::Level::LOG_WARNING, "VFS: Mount point %s already exists\n", path);
            return FALSE;
        }

        g_VFSLock.AcquireWrite();
        // Ensure partition is mounted (instantiate driver already done in Partition::Mount)
        FileSystem* fs = Part->GetFilesystem();
        if(!fs){
            Printk::Write(Printk::Level::LOG_ERR, "VFS: Partition has no filesystem for mount %s\n", path);
            return FALSE;
            g_VFSLock.ReleaseWrite();
        }
        // Record mount point
        String::Strncpy(g_MountPoints[g_MountPointCount].path, path, sizeof(g_MountPoints[g_MountPointCount].path)-1);
        g_MountPoints[g_MountPointCount].path[sizeof(g_MountPoints[g_MountPointCount].path)-1] = '\0';
        g_MountPoints[g_MountPointCount].fs = fs;
        g_MountPointCount++;
        Printk::Write(Printk::Level::LOG_INFO, "VFS: Mounted FS at %s\n", path);
        g_VFSLock.ReleaseWrite();
        return TRUE;
    }

    BOOL MountFS(const char *path, FileSystem* fs){
        if(!path || !fs) return FALSE;
        if(g_MountPointCount >= MAX_MOUNT_POINTS) return FALSE;
        if(path[0] != '/') return FALSE;
        if(FindMountIndex(path) >= 0){
            Printk::Write(Printk::Level::LOG_WARNING, "VFS: Mount point %s already exists\n", path);
            return FALSE;
        }
        String::Strncpy(g_MountPoints[g_MountPointCount].path, path, sizeof(g_MountPoints[g_MountPointCount].path)-1);
        g_MountPoints[g_MountPointCount].path[sizeof(g_MountPoints[g_MountPointCount].path)-1] = '\0';
        g_MountPoints[g_MountPointCount].fs = fs;
        g_MountPointCount++;
        Printk::Write(Printk::Level::LOG_INFO, "VFS: Mounted FS instance at %s\n", path);
        return TRUE;
    }

    // Not implemented auto-mount variant; stub returning FALSE for now
    BOOL Mount(Partition *Part){ (void)Part; return FALSE; }

    BOOL FindMountPoint(const char *path, FileSystem** outFS, char *OutRelativePath){
        if(outFS) *outFS = nullptr;
        if(OutRelativePath) OutRelativePath[0] = '\0';
        
        // 1. Validasi & Pre-Check (Asumsi Path sudah Canonical / bersih dari '..')
        if(!path || path[0] != '/') return FALSE;

        int bestIdx = -1; 
        unsigned long long bestLen = 0;

        // 2. Find Mount Point (Longest Prefix Match)
        for(U32 i = 0; i < g_MountPointCount; i++){
            const char* mp = g_MountPoints[i].path;
            unsigned long long mpl = String::Strlen(mp);
            
            // Optimasi: Kalau path user lebih pendek dari mount point, gak mungkin match
            // (Kecuali mount pointnya root)
            if (mpl > 1 && String::Strlen(path) < mpl) continue;

            if(String::Strncmp(path, mp, mpl) == 0) {
                BOOL isMatch = FALSE;

                // Logic Boundary Check lo udah bener banget disini
                if (mpl == 1 && mp[0] == '/') {
                    isMatch = TRUE; 
                }
                else if (path[mpl] == '\0' || path[mpl] == '/') {
                    isMatch = TRUE;
                }

                if (isMatch) {
                    if (mpl > bestLen || bestIdx == -1) {
                        bestIdx = (int)i;
                        bestLen = mpl;
                    }
                }
            }
        }

        if(bestIdx < 0) return FALSE;

        // 3. Output Assignment
        if(outFS) *outFS = g_MountPoints[bestIdx].fs;

        if(OutRelativePath){
            const char* rest = path + bestLen;
            
            // Logic path stripping
            // Kalau bestLen = 1 (Root "/"), path "/bin" -> rest "bin".
            // Kalau bestLen > 1 ("/mnt"), path "/mnt/bin" -> rest "/bin".
            
            // Kita mau hasil akhirnya selalu diawali '/'
            // Contoh target: "/bin"
            
            OutRelativePath[0] = '/';
            int writeOffset = 1;
            
            // Kalau rest diawali '/', skip biar gak jadi "//bin"
            if(rest[0] == '/') rest++;
            
            // Copy sisa string
            String::Strncpy(OutRelativePath + writeOffset, rest, 255 - writeOffset);
            
            // Safety Termination
            OutRelativePath[255] = '\0';
        }
        
        return TRUE;
    }

    BOOL ResolvePath(const char *path, FileSystem **outFS, char *OutRelativePath, BOOL FollowLastSymlink){
        ANSI_STRING CurrentPath[256];
        ANSI_STRING LinkTarget[256];
        ANSI_STRING TempPath[256];

        CanonicalizePath("/", path, CurrentPath);
        INTN LoopCount = 0;

        g_VFSLock.AcquireRead();

        while(LoopCount < MAX_SYMLINK_DEPTH){
            FileSystem* fs = nullptr;
            char rel[256];
            if (!FindMountPoint(CurrentPath, &fs, rel)) {
                g_VFSLock.ReleaseRead();
                return FALSE; // Gak ketemu mount point
            }

            FileInfo Info;

            if (!fs->Stat(rel, &Info)) {
                if(outFS) *outFS = fs;
                if(OutRelativePath) String::Strncpy(OutRelativePath, rel, 255);
                g_VFSLock.ReleaseRead();
                return TRUE; 
            }

            if (Info.Type == FT_SYMLINK) {
                
                // LOGIC UTAMA LSTAT VS STAT DISINI
                // Kalau ini link, dan user minta JANGAN di-follow (lstat), stop disini.
                if (!FollowLastSymlink) {
                    if(outFS) *outFS = fs;
                    if(OutRelativePath) String::Strncpy(OutRelativePath, rel, 255);
                    g_VFSLock.ReleaseRead();
                    return TRUE;
                }

                // Kalau user minta follow (stat/open), kita BACA isinya.
                // Driver harus implement ReadLink!
                I64 len = fs->ReadLink(rel, LinkTarget, 255);
                if (len < 0) {
                    g_VFSLock.ReleaseRead();
                    return FALSE;
                }
                LinkTarget[len] = '\0';

                // D. Rakit Path Baru
                if (LinkTarget[0] == '/') {
                    // Symlink Absolute: "/var" -> "/mnt/data/var"
                    // Kita ganti total current_path jadi target
                    String::Strncpy(CurrentPath, LinkTarget, 255);
                } else {
                    // Symlink Relative: "log" -> "../tmp/log"
                    // Ini agak tricky, kita harus gabungin (DirName current) + (Target)
                    // Implementasi simple:
                    // 1. Cari slash terakhir di current_path
                    CHAR8* last_slash = (CHAR8*)String::Strrchr(CurrentPath, '/');
                    if (last_slash) {
                        *(last_slash + 1) = '\0'; // Potong nama file lama
                    } else {
                        CurrentPath[0] = '/'; CurrentPath[1] = '\0';
                    }
                    
                    // 2. Gabungin
                    //String::Snprintf(TempPath, 255, "%s%s", CurrentPath, LinkTarget);
                    
                    // 3. Canonicalize lagi (biar ".." di tengah ilang)
                    CanonicalizePath("/", TempPath, CurrentPath);
                }

                LoopCount++;
                continue; // ULANGI LOOP DENGAN PATH BARU
            }

            if(outFS) *outFS = fs;
            if(OutRelativePath) String::Strncpy(OutRelativePath, rel, 255);
            g_VFSLock.ReleaseRead();
            return TRUE;
        }
        g_VFSLock.ReleaseRead();
        return FALSE; // ga ketemu apa apa kan?
    }

    File* Open(const char* path, U32 Flags){
        FileSystem *Fs = nullptr;
        CHAR8 Rel[256];

        if(!ResolvePath(path, &Fs, Rel)) {
            Printk::Write(Printk::Level::LOG_ERR, "VFS: Open failed to resolve path '%s'\n", path);
            return nullptr;
        }

        U64 InodeNum = Fs->Lookup(Rel);

        if(InodeNum == 0 && (Flags & O_CREAT)) {
            InodeNum = Fs->CreateNode(Rel, Flags);
            if(InodeNum == 0) {
                Printk::Write(Printk::Level::LOG_ERR, "VFS: Open failed to create node for path '%s'\n", path);
                return nullptr;
            }
        } else if (InodeNum == 0) {
            Printk::Write(Printk::Level::LOG_ERR, "VFS: Open failed to find node for path '%s'\n", path);
            return nullptr;
        }

        g_VFSLock.AcquireWrite();
        ::Inode *VFSNode = GetCachedInode(Fs, InodeNum);

        if(VFSNode != nullptr) {
            VFSNode->RefCount++;
            g_VFSLock.ReleaseWrite();
        } else {
            VFSNode = new ::Inode();
            if(!VFSNode) return nullptr;

            VFSNode->FSOwner = Fs;
            VFSNode->InodeID = InodeNum;
            VFSNode->RefCount = 1;

            Fs->PopulateInode(InodeNum, VFSNode);

            AddCachedInode(VFSNode);
            g_VFSLock.ReleaseWrite();
        }

        File *file = new File();
        if(!file) {
            VFSNode->RefCount--;
            Printk::Write(Printk::Level::LOG_ERR, "VFS: Open failed to allocate File object for path '%s'\n", path);
            return nullptr;
        }

        String::Strcpy(file->FileName, path);
        file->CurrentPosition = 0;
        file->Flags = Flags;
        file->Node = VFSNode; // Hubungkan ke Inode
        file->RefCount = 1;

        if (Flags & O_TRUNC) {
            Fs->Truncate(file, 0); // O_TRUNC dikerjain kuli
        }

        return file;
    }

    // Ensure parent directories exist inside the filesystem for the given relative path.
    // This is a conservative, debug-friendly helper that implements mkdir -p semantics.
    static BOOL EnsureParentDirs(FileSystem* fs, const char* relPath){
        if(!fs || !relPath) return FALSE;
        U32 len = (U32)String::Strlen(relPath);
        if(len == 0) return TRUE;

        const char* last = nullptr;
        for(U32 i=0;i<len;i++) if(relPath[i] == '/') last = &relPath[i];
        if(!last || last == &relPath[0]) return TRUE;

        CHAR8 parent[256]; U32 pLen = (U32)(last - relPath);
        if(pLen >= sizeof(parent)) return FALSE;
        String::Memcpy(parent, relPath, pLen); parent[pLen] = '\0';

        CHAR8 accum[256]; accum[0] = '/'; accum[1] = '\0';
        U32 pos = 1;
        U32 i = 1; 

        while(i <= pLen){
            U32 j = i;
            while(j < pLen && parent[j] != '/') j++;
            U32 compLen = j - i;
            if(compLen == 0){ i = j+1; continue; }
            if(pos + 1 + compLen >= sizeof(accum)) return FALSE;
            
            if(accum[pos-1] != '/') { accum[pos++] = '/'; accum[pos] = '\0'; }
            String::Memcpy(accum + pos, parent + i, compLen);
            pos += compLen; accum[pos] = '\0';

            // ========================================================
            // CARA BARU NGECEK FOLDER (GAK PERLU BIKIN FILE OBJECT)
            // ========================================================
            U32 inodeId = fs->Lookup(accum);
            
            if(inodeId != 0) {
                // Folder/File ketemu di disk!
                // Cek apakah dia beneran folder? Minta info speknya:
                ::Inode tempNode;
                if(fs->PopulateInode(inodeId, &tempNode)){
                    if(tempNode.Type != FT_DIR) return FALSE; // Nabrak, ternyata itu file biasa
                }
            } else {
                // Gak ada di disk, kita coba bikin (MKDir)
                if(!fs->MKDir(accum)){
                    // Kalo gagal, cek lagi siapa tau ada program lain yang baru aja bikin (Race Condition)
                    inodeId = fs->Lookup(accum);
                    if(inodeId == 0) return FALSE; 
                    
                    ::Inode tempNode;
                    if(!fs->PopulateInode(inodeId, &tempNode) || tempNode.Type != FT_DIR) {
                        return FALSE; 
                    }
                }
            }

            i = j + 1;
        }
        return TRUE;
    }

    // Create with parent directories created as needed
    File* CreateWithParents(const char *Path){
        FileSystem* fs = nullptr; char rel[256];
        
        if(!ResolvePath(Path, &fs, rel)){
            Printk::Write(Printk::Level::LOG_DEBUG, "VFS: CreateWithParents - ResolvePath failed for '%s'\n", Path);
            return nullptr;
        }
        
        if(!EnsureParentDirs(fs, rel)){
            Printk::Write(Printk::Level::LOG_WARNING, "VFS: CreateWithParents - failed to ensure parents for '%s'\n", rel);
            return nullptr;
        }
        
        // Langsung panggil VFSManager::Open pakai Path awal!
        return Open(Path, O_RDWR | O_CREAT);
    }

    VOID Close(File* file) {
        if (!file) return;

        // 1. Cek RefCount sesi File (PENTING BANGET buat Fork/Dup!)
        file->RefCount--;
        if (file->RefCount > 0) {
            return; // Jangan diapa-apain, FD ini masih dipake di tempat lain!
        }

        // ==========================================
        // 2. JALUR KHUSUS SHM
        // ==========================================
        if (file->type == FileType::FT_SHM) {
            ShmRegion* Region = (ShmRegion*)file->PrivateData;
            
            if (Region) {
                SharedMemoryManager::Release(Region);
            }

            // Langsung delete object File-nya dan RETURN.
            // JANGAN biarin dia turun ke bawah, karena Inode SHM
            // itu urusannya SharedMemoryManager, bukan urusan VFS!
            delete file;
            return;
        }

        // ==========================================
        // 3. JALUR NORMAL (File Disk / Direktori)
        // ==========================================
        FileSystem* fs = file->Node ? file->Node->FSOwner : nullptr;

        if (fs) {
            fs->Close(file); // Kasih tau driver (FAT32/Ext2) buat ngelepas file
        }

        // Cleanup Inode (Tersangka 1 lu udah aman sekarang)
        if (file->Node) {
            file->Node->RefCount--; 
            if (file->Node->RefCount <= 0) {
                // Kalau udah gak ada file yang nunjuk ke Inode ini, hapus dari cache & RAM
                RemoveCachedInode(fs, file->Node->InodeID); 
                delete file->Node; 
            }
        } 

        // Tersangka 2 lu juga aman, karena File RefCount udah 0 di cek nomor 1
        delete file; 
    }

    U32 Read(File* file, U8* buffer, U32 size){
        if(!file) {
            Printk::Write(Printk::Level::LOG_ERR, "VFS: Read called with NULL File object\n");
            return 0;
        }

        if(file->type == FT_PIPE){
            return PipeFileSystem::GetInstance()->Read(file, buffer, size);
        }
        

        if(!file->Node || !file->Node->FSOwner) {
            Printk::Write(Printk::Level::LOG_ERR, "VFS: Read failed - File object has no valid node or filesystem owner\n");
            return 0;
        }

        file->Node->Lock.Acquire();
        U32 bytesRead = file->Node->FSOwner->Read(file, buffer, size);
        file->Node->Lock.Release();
        
        return bytesRead; 
    }

    U32 Write(File *FileObj, U8 *Buffer, U32 Size){ 
        if(!FileObj) return 0;

        if(FileObj->type == FT_PIPE){
            return PipeFileSystem::GetInstance()->Write(FileObj, Buffer, Size);
        }

        if(!FileObj->Node || !FileObj->Node->FSOwner) return 0;

        FileObj->Node->Lock.Acquire();
        U32 written = FileObj->Node->FSOwner->Write(FileObj, Buffer, Size);
        FileObj->Node->Lock.Release();
        
        return written;
    }
    // Debug wrapper to trace VFS write calls (helps ensure caller passes proper File* and FSOwner)
    U32 DebugWrite(File *FileObj, U8 *Buffer, U32 Size){
        if(!FileObj){ Printk::Write(Printk::Level::LOG_DEBUG, "VFS: DebugWrite called with NULL FileObj\n"); return 0; }
        
        if(FileObj->type == FT_PIPE) return PipeFileSystem::GetInstance()->Write(FileObj, Buffer, Size);
        
        if(!FileObj->Node || !FileObj->Node->FSOwner){ 
            Printk::Write(Printk::Level::LOG_ERR, "VFS: DebugWrite - FileObj has no FSOwner\n"); return 0; 
        }

        FileObj->Node->Lock.Acquire();
        U32 written = FileObj->Node->FSOwner->Write(FileObj, Buffer, Size);
        FileObj->Node->Lock.Release();
        return written;
    }

    U32 Append(const char* path, U8* Buffer, U32 Size){
        if(!path || !Buffer || Size == 0) return 0;
        
        // Langsung panggil VFSManager::Open (fungsi Open yang lu bikin di vfs.cpp)
        // Dia butuh absolute path, jadi langsung passing 'path' aja!
        File* f = Open(path, O_RDWR | O_APPEND | O_CREAT);
        if(!f) return 0; 
        
        // PENTING: Cek IsDirectory sekarang harus lewat Node-nya!
        if(f->Node && f->Node->Type == FT_DIR){ 
            Close(f); 
            return 0; 
        }
        
        // Panggil Seek (FileSize sekarang ada di Node)
        if (f->Node) {
            (void)Seek(f, f->Node->FileSize, SEEK_END);
        }
        
        // Panggil VFSManager::Write biar otomatis ke-lock!
        U32 written = Write(f, Buffer, Size); 
        Close(f);
        return written;
    }

    BOOL Delete(const char* path){
        FileSystem* fs = nullptr; char rel[256];
        if(!ResolvePath(path, &fs, rel)) return FALSE;
        return fs->Delete(rel);
    }

    BOOL Rename(const char* oldPath, const char* newPath){
        FileSystem* fsOld=nullptr; char relOld[256];
        FileSystem* fsNew=nullptr; char relNew[256];
        if(!ResolvePath(oldPath, &fsOld, relOld)) return FALSE;
        if(!ResolvePath(newPath, &fsNew, relNew)) return FALSE;
        if(fsOld != fsNew){
            // Cross-filesystem rename not supported
            return FALSE;
        }
        return fsOld->Rename(relOld, relNew);
    }

    BOOL Seek(File* file, U64 position, U32 origin){
        if(!file || file->type == FT_PIPE) return FALSE; // Pipe gak bisa di-seek
        if(!file->Node || !file->Node->FSOwner) return FALSE;
        
        file->Node->Lock.Acquire();
        BOOL result = file->Node->FSOwner->Seek(file, position, origin);
        file->Node->Lock.Release();
        return result;
    }

    BOOL Truncate(File* file, U64 size){
        if(!file || file->type == FT_PIPE) return FALSE;
        if(!file->Node || !file->Node->FSOwner) return FALSE;
        
        file->Node->Lock.Acquire();
        BOOL result = file->Node->FSOwner->Truncate(file, size);
        file->Node->Lock.Release();
        return result;
    }

    BOOL MKDir(const char* path){
        FileSystem* fs = nullptr; char rel[256];
        if(!ResolvePath(path, &fs, rel)) return FALSE;
        return fs->MKDir(rel);
    }

    BOOL RMDir(const char* path){
        FileSystem* fs = nullptr; char rel[256];
        if(!ResolvePath(path, &fs, rel)) return FALSE;
        return fs->RMDir(rel);
    }

    BOOL Flush(File* file){
        if(!file || file->type == FT_PIPE) return FALSE;
        if(!file->Node || !file->Node->FSOwner) return FALSE;
        
        file->Node->Lock.Acquire();
        BOOL result = file->Node->FSOwner->Flush(file);
        file->Node->Lock.Release();
        return result;
    }

    INTN ReadDir(File* dirFile, void* buffer, U32 bufferSize){
        if(!dirFile || dirFile->type == FT_PIPE) return -1;
        if(!dirFile->Node || !dirFile->Node->FSOwner) return -1;
        
        dirFile->Node->Lock.Acquire();
        INTN result = dirFile->Node->FSOwner->ReadDir(dirFile, buffer, bufferSize);
        dirFile->Node->Lock.Release();
        return result;
    }

    INTN Ioctl(File* file, U32 command, U64 arg){
        if(!file || file->type == FT_PIPE) return -1;
        if(!file->Node || !file->Node->FSOwner) return -1;
        
        file->Node->Lock.Acquire();
        INTN result = file->Node->FSOwner->Ioctl(file, command, arg);
        file->Node->Lock.Release();
        return result;
    }

    BOOL SyncAll(){
        // Implemnetasi awal
        // SYNC ke semua Controller storage yang terdaftar di sistem
        // Nanti kalau ada FS yang butuh flush khusus, bisa ditambahin di sini
        // Contoh: EXT2, FAT32, dll

        DeviceManager::StorageManager::SyncAllStorageDevices();

        return TRUE;
    }
}