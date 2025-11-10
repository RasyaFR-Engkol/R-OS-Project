#define PRINTK_MODULE_NAME "VFS"
#include "vfs.hpp"
#include <string.hpp>
#include <logging.hpp>

#define MAX_FS_DRIVERS 10
#define MAX_MOUNT_POINTS 20

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

    // Not implemented auto-mount variant; stub returning FALSE for now
    BOOL Mount(Partition *Part){ (void)Part; return FALSE; }

    BOOL ResolvePath(const char *path, FileSystem** outFS, char *OutRelativePath){
        if(outFS) {
            *outFS = nullptr;
        }
        if(OutRelativePath) {
            OutRelativePath[0] = '\0';
        }
        if(!path || path[0] != '/') return FALSE;
        // Longest-prefix match among mount points
        int bestIdx = -1; unsigned long long bestLen = 0;
        for(U32 i=0;i<g_MountPointCount;i++){
            const char* mp = g_MountPoints[i].path;
            unsigned long long mpl = String::Strlen(mp);
            if(mpl > bestLen){
                // Must match prefix and either exact or next char is '/'
                if(String::Strncmp(path, mp, mpl) == 0 && (path[mpl] == '\0' || path[mpl] == '/')){
                    bestIdx = (int)i; bestLen = mpl;
                }
            }
        }
        if(bestIdx < 0) return FALSE;
        if(outFS) *outFS = g_MountPoints[bestIdx].fs;
        if(OutRelativePath){
            const char* rest = path + bestLen;
            if(rest[0] == '/') rest++; // skip separator
            // copy remainder (may be empty -> root inside FS)
            String::Strncpy(OutRelativePath, rest, 255);
            OutRelativePath[255] = '\0';
            if(OutRelativePath[0] != '/'){
                // underlying FS expects paths starting with '/'; add leading slash
                // shift right; ensure space
                unsigned long long cur = String::Strlen(OutRelativePath);
                if(cur + 1 < 256){
                    for(long long i = (long long)cur; i >= 0; --i){ // include NUL
                        OutRelativePath[i+1] = OutRelativePath[i];
                    }
                    OutRelativePath[0] = '/';
                }
            }
        }
        return TRUE;
    }

    File* Open(const char* path){
        FileSystem* fs = nullptr; char rel[256];
        if(!ResolvePath(path, &fs, rel)) return nullptr;
        return fs->Open(rel);
    }

    File* Create(const char *Path){
        FileSystem* fs = nullptr; char rel[256];
        if(!ResolvePath(Path, &fs, rel)) return nullptr;
        return fs->Create(rel);
    }

    void Close(File* file){
        if(!file) return;
        if(!file->FSOwner) {
            return;
        }
        file->FSOwner->Close(file);
    }

    U32 Read(File* file, U8* buffer, U32 size){ if(!file || !file->FSOwner) return 0; return file->FSOwner->Read(file, buffer, size); }
    U32 Write(File *FileObj, U8 *Buffer, U32 Size){ if(!FileObj || !FileObj->FSOwner) return 0; return FileObj->FSOwner->Write(FileObj, Buffer, Size); }

    U32 Append(const char* path, U8* Buffer, U32 Size){
        if(!path || !Buffer || Size == 0) return 0;
        FileSystem* fs = nullptr; char rel[256];
        if(!ResolvePath(path, &fs, rel)) return 0;
        // Try to open existing; if not exists, create
        File* f = fs->Open(rel);
        if(!f){
            f = fs->Create(rel);
            if(!f) return 0;
        }
        if(f->IsDirectory){ fs->Close(f); return 0; }
        // Seek to end
        (void)Seek(f, f->FileSize);
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

    BOOL Seek(File* file, U64 position){
        if(!file || !file->FSOwner) return FALSE;
        // Delegate to filesystem so it can update cluster cursor/state correctly
        return file->FSOwner->Seek(file, position);
    }


}