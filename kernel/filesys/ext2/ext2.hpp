#pragma once
#include <rosval.h>
#include "../filesystem.hpp"
#include "../partition.hpp"
#include "../iblockdevice.hpp"

#define EXT2_SUPER_MAGIC 0xEF53

namespace EXT2{
    struct SuperBlock{
        U32 s_inodes_count;      // Total inodes
        U32 s_blocks_count;      // Total blocks
        U32 s_r_blocks_count;    // Reserved blocks (for super-user)
        U32 s_free_blocks_count; // Free blocks
        U32 s_free_inodes_count; // Free inodes
        U32 s_first_data_block;  // 0 untuk block size > 1KB, 1 untuk 1KB
        U32 s_log_block_size;    // Block size (dihitung sbg 1024 << s_log_block_size)
        U32 s_log_frag_size;     // Fragment size
        U32 s_blocks_per_group;  // Blocks per group
        U32 s_frags_per_group;   // Fragments per group
        U32 s_inodes_per_group;  // Inodes per group
        U32 s_mtime;             // Mount time
        U32 s_wtime;             // Write time
        U16 s_mnt_count;         // Mount count
        U16 s_max_mnt_count;     // Max mount count
        U16 s_magic;             // **INI PENTING! Harus 0xEF53**
        U16 s_state;             // File system state
        U16 s_errors;            // Behaviour when detecting errors
        U16 s_minor_rev_level;   // Minor revision level
        U32 s_lastcheck;         // Time of last check
        U32 s_checkinterval;     // Max time between checks
        U32 s_creator_os;        // Creator OS
        U32 s_rev_level;         // Revision level
        U16 s_def_resuid;        // Default UID for reserved blocks
        U16 s_def_resgid;        // Default GID for reserved blocks
        
        // -- Bidang EXT2_DYNAMIC_REV --
        U32 s_first_ino;         // First non-reserved inode
        U16 s_inode_size;        // Ukuran struktur inode
        U16 s_block_group_nr;    // Block group # of this superblock
        U32 s_feature_compat;    // Compatible feature set
        U32 s_feature_incompat;  // Incompatible feature set
        U32 s_feature_ro_compat; // Read-only compatible feature set
        U8  s_uuid[16];          // 128-bit uuid
        char s_volume_name[16];  // Volume name
        char s_last_mounted[64]; // Directory where last mounted
        U32 s_algo_bitmap;       // For compression
        // ...dan seterusnya, tapi ini cukup untuk mulai...
        U8  padding[824]; // Padding agar total 1024 bytes
    } PACKSTRUCT;

    VOID InitializeEXT2Driver();
} 