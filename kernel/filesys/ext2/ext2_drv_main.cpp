#include <rosval.h>
#include "ext2_driver.hpp"
#include "ext2.hpp"
#include "../vfs/vfs.hpp"
#define PRINTK_MODULE_NAME "EXT2FS"
#include <logging.hpp>

namespace EXT2{
    FileSystem *CreateEXT2Driver(){
        Printk::Write(Printk::Level::LOG_INFO, "EXT2: Creating EXT2 filesystem driver instance.\n");
        return new EXT2FileSystem();
    }

    VOID InitializeEXT2Driver(){
        VFSManager::RegisterFileSystem("EXT2", &CreateEXT2Driver);
    }
}