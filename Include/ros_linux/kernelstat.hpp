#pragma once
#include <rosval.h>

struct kernel_stat {
    U64 st_dev;     // Device ID
    U64 st_ino;     // Inode number
    U64 st_nlink;   // Hard link count
    U32 st_mode;    // Permission & File Type (PENTING!)
    U32 st_uid;     // User ID
    U32 st_gid;     // Group ID
    U32 __pad0;
    U64 st_rdev;    // Device ID (if special file)
    I64 st_size;    // Total size, in bytes
    I64 st_blksize; // Block size for filesystem I/O
    I64 st_blocks;  // Number of 512B blocks allocated
    // Timestamps (Seconds & Nanoseconds)
    U64 st_atime_sec; U64 st_atime_nsec;
    U64 st_mtime_sec; U64 st_mtime_nsec;
    U64 st_ctime_sec; U64 st_ctime_nsec;
    I64 __unused[3];
};