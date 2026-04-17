#include "filesystem.hpp"

BOOL FileSystem::Truncate(File* file, U64 size){
    (void)file; (void)size;
    return FALSE;
}

BOOL FileSystem::MKDir(const char* path){
    (void)path;
    return FALSE;
}

BOOL FileSystem::RMDir(const char* path){
    (void)path;
    return FALSE;
}

BOOL FileSystem::Flush(File* file){
    (void)file;
    return FALSE;
}

