#include "rosval.h"
#define PRINTK_MODULE_NAME "VFS"
#include "vfs.hpp"
#include <string.hpp>
#include <logging.hpp>
#include "../../dev/devicemanager.hpp"

#define MAX_FS_DRIVERS 10
#define MAX_MOUNT_POINTS 20

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
        // Ensure partition is mounted (instantiate driver already done in Partition::Mount)
        FileSystem* fs = Part->GetFilesystem();
        if(!fs){
            Printk::Write(Printk::Level::LOG_ERR, "VFS: Partition has no filesystem for mount %s\n", path);
            return FALSE;
        }
        // Record mount point
        String::Strncpy(g_MountPoints[g_MountPointCount].path, path, sizeof(g_MountPoints[g_MountPointCount].path)-1);
        g_MountPoints[g_MountPointCount].path[sizeof(g_MountPoints[g_MountPointCount].path)-1] = '\0';
        g_MountPoints[g_MountPointCount].fs = fs;
        g_MountPointCount++;
        Printk::Write(Printk::Level::LOG_INFO, "VFS: Mounted FS at %s\n", path);
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

        while(LoopCount < MAX_SYMLINK_DEPTH){
            FileSystem* fs = nullptr;
            char rel[256];
            if (!FindMountPoint(CurrentPath, &fs, rel)) {
                return FALSE; // Gak ketemu mount point
            }

            FileInfo Info;

            if (!fs->Stat(rel, &Info)) {
                if(outFS) *outFS = fs;
                if(OutRelativePath) String::Strncpy(OutRelativePath, rel, 255);
                return TRUE; 
            }

            if (Info.Type == FT_SYMLINK) {
                
                // LOGIC UTAMA LSTAT VS STAT DISINI
                // Kalau ini link, dan user minta JANGAN di-follow (lstat), stop disini.
                if (!FollowLastSymlink) {
                    if(outFS) *outFS = fs;
                    if(OutRelativePath) String::Strncpy(OutRelativePath, rel, 255);
                    return TRUE;
                }

                // Kalau user minta follow (stat/open), kita BACA isinya.
                // Driver harus implement ReadLink!
                I64 len = fs->ReadLink(rel, LinkTarget, 255);
                if (len < 0) return FALSE; // Error baca link
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
            return TRUE;
        }
        return FALSE; // ga ketemu apa apa kan?
    }

    File* Open(const char* path, U32 Flags){
        FileSystem* fs = nullptr; char rel[256];
        if(!ResolvePath(path, &fs, rel)) {
            Printk::Write(Printk::Level::LOG_ERR, "VFS: Open - ResolvePath failed for '%s'\n", path);
            return nullptr;
        }
        return fs->Open(rel, Flags);
    }

    // Ensure parent directories exist inside the filesystem for the given relative path.
    // This is a conservative, debug-friendly helper that implements mkdir -p semantics.
    static BOOL EnsureParentDirs(FileSystem* fs, const char* relPath){
        if(!fs || !relPath) return FALSE;
        // relPath expected to start with '/'
        U32 len = (U32)String::Strlen(relPath);
        if(len == 0) return TRUE;
        // find parent portion (strip trailing component)
        // e.g. relPath = "/a/b/c.txt" -> parent = "/a/b"
        const char* last = nullptr;
        for(U32 i=0;i<len;i++) if(relPath[i] == '/') last = &relPath[i];
        // If only root or no slash, nothing to do
        // But relPath always begins with '/', so if last == &relPath[0], parent is root
        if(!last || last == &relPath[0]) return TRUE;
        // Copy parent path
        CHAR8 parent[256]; U32 pLen = (U32)(last - relPath);
        if(pLen >= sizeof(parent)) return FALSE;
        String::Memcpy(parent, relPath, pLen); parent[pLen] = '\0';

        // Iterate path components from top to bottom, creating as needed
        CHAR8 accum[256]; accum[0] = '/'; accum[1] = '\0';
        U32 pos = 1;
        U32 i = 1; // skip initial '/'
        while(i <= pLen){
            // find next slash or end
            U32 j = i;
            while(j < pLen && parent[j] != '/') j++;
            U32 compLen = j - i;
            if(compLen == 0){ i = j+1; continue; }
            if(pos + 1 + compLen >= sizeof(accum)) return FALSE;
            // append component
            if(accum[pos-1] != '/') { accum[pos++] = '/'; accum[pos] = '\0'; }
            String::Memcpy(accum + pos, parent + i, compLen);
            pos += compLen; accum[pos] = '\0';

            // Check if exists by trying to open
            File* f = fs->Open(accum, O_RDWR);
            if(f){
                // exists; ensure it's a directory
                if(!f->IsDirectory){ fs->Close(f); return FALSE; }
                fs->Close(f);
            } else {
                // try to create directory
                if(!fs->MKDir(accum)){
                    // If MKDir failed, maybe it now exists (race) -> try Open again
                    File* f2 = fs->Open(accum, O_RDWR);
                    if(!f2) return FALSE;
                    if(!f2->IsDirectory){ fs->Close(f2); return FALSE; }
                    fs->Close(f2);
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
        return fs->Open(rel, O_RDWR | O_CREAT);
    }

    void Close(File* file){
        if(!file) return;
        if(file->FSOwner) {
            file->FSOwner->Close(file);
        } else {
            // No filesystem owner: free the File object directly.
            delete file;
        }
    }

    U32 Read(File* file, U8* buffer, U32 size){ if(!file || !file->FSOwner) return 0; return file->FSOwner->Read(file, buffer, size); }
    U32 Write(File *FileObj, U8 *Buffer, U32 Size){ if(!FileObj || !FileObj->FSOwner) return 0; return FileObj->FSOwner->Write(FileObj, Buffer, Size); }

    // Debug wrapper to trace VFS write calls (helps ensure caller passes proper File* and FSOwner)
    U32 DebugWrite(File *FileObj, U8 *Buffer, U32 Size){
        if(!FileObj){ Printk::Write(Printk::Level::LOG_DEBUG, "VFS: DebugWrite called with NULL FileObj\n"); return 0; }
        Printk::Write(Printk::Level::LOG_DEBUG, "VFS: DebugWrite FileObj=%p FSOwner=%p Size=%u\n", (void*)FileObj, (void*)FileObj->FSOwner, Size);
        if(!FileObj->FSOwner){ Printk::Write(Printk::Level::LOG_ERR, "VFS: DebugWrite - FileObj has no FSOwner\n"); return 0; }
        return FileObj->FSOwner->Write(FileObj, Buffer, Size);
    }

    U32 Append(const char* path, U8* Buffer, U32 Size){
        if(!path || !Buffer || Size == 0) return 0;
        FileSystem* fs = nullptr; char rel[256];
        if(!ResolvePath(path, &fs, rel)) return 0;
        // Try to open existing; if not exists, create
        File* f = fs->Open(rel, O_RDWR | O_APPEND | O_CREAT);
        if(f->IsDirectory){ fs->Close(f); return 0; }
        // Seek to end
        (void)Seek(f, f->FileSize, SEEK_END);
        U32 written = fs->Write(f, Buffer, Size);
        fs->Close(f);
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
        if(!file || !file->FSOwner) return FALSE;
        // Delegate to filesystem so it can update cluster cursor/state correctly
        return file->FSOwner->Seek(file, position, origin);
    }

    BOOL Truncate(File* file, U64 size){
        if(!file || !file->FSOwner) return FALSE;
        return file->FSOwner->Truncate(file, size);
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
        if(!file || !file->FSOwner) return FALSE;
        return file->FSOwner->Flush(file);
    }

    INTN ReadDir(File* dirFile, void* buffer, U32 bufferSize){
        if(!dirFile || !dirFile->FSOwner) return -1;
        return dirFile->FSOwner->ReadDir(dirFile, buffer, bufferSize);
    }

    INTN Ioctl(File* file, U32 command, U64 arg){
        if(!file || !file->FSOwner) return -1;
        return file->FSOwner->Ioctl(file, command, arg);
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