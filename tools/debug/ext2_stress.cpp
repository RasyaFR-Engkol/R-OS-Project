#include <rosval.h>
#define PRINTK_MODULE_NAME "EXT2_STRESS"
#include <logging.hpp>
#include <string.hpp>
#include <mm.hpp>
#include "../../kernel/filesys/vfs/vfs.hpp"
#include "../../kernel/filesys/ext2/ext2_driver.hpp"

ABI_C VAL32 main_debug_ext2_stress(int argc, char** argv){
    Printk::Write(Printk::Level::LOG_INFO, "EXT2_STRESS: starting\n");

    const char* mountPrefix = "/mnt/part1";
    FileSystem* fs = nullptr;
    char rel[256];
    if(!VFSManager::ResolvePath(mountPrefix, &fs, rel)){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2_STRESS: ResolvePath failed for %s\n", mountPrefix);
        return 1;
    }

    EXT2FileSystem* ext2 = nullptr;
    if(fs){
        ext2 = static_cast<EXT2FileSystem*>(fs);
    }
    if(!ext2){
        Printk::Write(Printk::Level::LOG_ERR, "EXT2_STRESS: underlying FS is not EXT2 or not accessible\n");
        return 1;
    }

    // Create stress directory
    const char* stressDir = "/mnt/part1/stressdir";
    if(!VFSManager::MKDir(stressDir)){
        Printk::Write(Printk::Level::LOG_INFO, "EXT2_STRESS: MKDir may have failed or already exists\n");
    }

    // Configure failure injection: fail after N writes (tunable)
    U32 failAfter = 0;
    if(argc > 1){
        // simple integer parse
        failAfter = 0;
        const char* s = argv[1];
        for(; *s; ++s){ if(*s >= '0' && *s <= '9') { failAfter = failAfter*10 + (U32)(*s - '0'); } }
    }
    if(failAfter > 0){
        ext2->DebugSetFailAfter(failAfter);
        Printk::Write(Printk::Level::LOG_INFO, "EXT2_STRESS: Enabled debug fail-after=%u\n", failAfter);
    }

    // Number of files to create; large enough to grow into indirect blocks depending on block size
    U32 filesToCreate = 100; // adjustable
    if(argc > 2){ filesToCreate = 0; const char* s2 = argv[2]; for(; *s2; ++s2){ if(*s2 >= '0' && *s2 <= '9') filesToCreate = filesToCreate*10 + (U32)(*s2 - '0'); } }

    char path[512];
    U32 created = 0;
    for(U32 i = 0; i < filesToCreate; i++){
        // build filename: /mnt/part1/stressdir/fileNNNNNN.txt
        String::Strcpy(path, "/mnt/part1/stressdir/file");
        // append zero-padded 6-digit number
        char numbuf[16];
        for(int d=0; d<6; d++) numbuf[d] = '0';
        numbuf[6] = '\0';
        U32 v = i;
        for(int p=5; p>=0; p--){ numbuf[p] = '0' + (v % 10); v /= 10; }
        String::Strcat(path, numbuf);
        String::Strcat(path, ".txt");
    File* f = VFSManager::Create(path);
        if(!f){
            Printk::Write(Printk::Level::LOG_WARNING, "EXT2_STRESS: Create failed for %s (i=%u)\n", path, i);
            // continue; maybe failure injection triggered
            continue;
        }
        // write small payload to ensure block allocation
        const char* payload = "hello\n";
        VFSManager::Write(f, (U8*)payload, (U32)String::Strlen(payload));
        VFSManager::Close(f);
        created++;
        if((i & 0x3FF) == 0){ // every 1024 iterations, print progress
            Printk::Write(Printk::Level::LOG_INFO, "EXT2_STRESS: created %u/%u\n", created, filesToCreate);
        }
    }

    Printk::Write(Printk::Level::LOG_INFO, "EXT2_STRESS: finished creation loop; created=%u requested=%u\n", created, filesToCreate);

    // Reset failure injection so that consistency check can read/write without forced failures
    ext2->DebugResetFail();

    // Run consistency check
    BOOL ok = ext2->DebugConsistencyCheck();
    if(ok){
        Printk::Write(Printk::Level::LOG_INFO, "EXT2_STRESS: Consistency check passed\n");
    } else {
        Printk::Write(Printk::Level::LOG_ERR, "EXT2_STRESS: Consistency check reported issues\n");
        // Try a conservative repair and re-run the check
        U32 fixes = ext2->DebugRepairConsistency();
        Printk::Write(Printk::Level::LOG_INFO, "EXT2_STRESS: Repair applied %u fixes, re-checking...\n", fixes);
        BOOL ok2 = ext2->DebugConsistencyCheck();
        if(ok2){
            Printk::Write(Printk::Level::LOG_INFO, "EXT2_STRESS: Consistency check passed after repair\n");
        } else {
            Printk::Write(Printk::Level::LOG_ERR, "EXT2_STRESS: Consistency check still reports issues after repair\n");
        }
    }

    // Attempt to remove stress directory (best-effort)
    if(VFSManager::RMDir("/mnt/part1/stressdir")){
        Printk::Write(Printk::Level::LOG_INFO, "EXT2_STRESS: Removed /mnt/part1/stressdir successfully\n");
    } else {
        Printk::Write(Printk::Level::LOG_WARNING, "EXT2_STRESS: Failed to remove /mnt/part1/stressdir - directory may be non-empty or inconsistent\n");
    }

    return 0;
}
