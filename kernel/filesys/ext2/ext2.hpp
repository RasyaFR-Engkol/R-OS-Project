#pragma once
#include <rosval.h>
#include "../partition.hpp"

#define EXT2_SUPER_MAGIC 0xEF53
#define EXT2_N_BLOCKS 15

// Mode mode file
#define EXT2_S_IFDIR  0x4000  // Directory
#define EXT2_S_IFREG  0x8000  // Regular file
#define EXT2_S_IFLNK  0xA000

#define EXT2_FT_UNKNOWN  0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2
#define EXT2_FT_CHRDEV   3
#define EXT2_FT_BLKDEV   4
#define EXT2_FT_FIFO     5
#define EXT2_FT_SOCK     6
#define EXT2_FT_SYMLINK  7

/* Mask untuk tipe file */
#define EXT2_S_IFMT   0xF000

/* Nilai untuk masing-masing tipe file */
#define EXT2_S_IFSOCK 0xC000  /* Socket */
#define EXT2_S_IFLNK  0xA000  /* Symbolic link */
#define EXT2_S_IFREG  0x8000  /* Regular file */
#define EXT2_S_IFBLK  0x6000  /* Block device */
#define EXT2_S_IFDIR  0x4000  /* Directory */
#define EXT2_S_IFCHR  0x2000  /* Character device */
#define EXT2_S_IFIFO  0x1000  /* FIFO */

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

    struct BlockGroupDescriptor {
        U32 bg_block_bitmap;        // Block # dari block bitmap
        U32 bg_inode_bitmap;        // Block # dari inode bitmap
        U32 bg_inode_table;         // Block # dari awal inode table
        U16 bg_free_blocks_count;   // Free blocks di grup ini
        U16 bg_free_inodes_count;   // Free inodes di grup ini
        U16 bg_used_dirs_count;     // Jumlah direktori di grup ini
        U16 bg_pad;
        U8  bg_reserved[12];
    } PACKSTRUCT;

    struct Inode {
        U16 i_mode;        // Tipe file (directory, file, link) dan permissions
        U16 i_uid;         // User ID
        U32 i_size;        // Ukuran file dalam bytes
        U32 i_atime;       // Access time
        U32 i_ctime;       // Creation time
        U32 i_mtime;       // Modification time
        U32 i_dtime;       // Deletion time
        U16 i_gid;         // Group ID
        U16 i_links_count; // Jumlah hard links
        U32 i_blocks;      // Jumlah *sektor* 512B (bukan block filesystem!)
        U32 i_flags;       // Flags
        U32 i_osd1;        // OS specific
        U32 i_block[EXT2_N_BLOCKS]; // **PENTING! Pointer ke data blocks**
                                    // [0-11] = Direct blocks
                                    // [12]   = Singly Indirect
                                    // [13]   = Doubly Indirect
                                    // [14]   = Triply Indirect
        U32 i_generation;
        U32 i_file_acl;
        U32 i_dir_acl;
        U32 i_faddr;
        U8  i_osd2[12];
    } PACKSTRUCT;

    struct DirectoryEntry {
        U32 inode;         // Nomor inode
        U16 rec_len;       // Panjang total entri ini
        U8  name_len;      // Panjang nama (N)
        U8  file_type;     // Tipe file
        char name[255];    // Nama file (panjangnya N, tidak NUL-terminated)
                        // Ukuran struct ini *variabel*
    };

    VOID InitializeEXT2Driver();
} 