#pragma once
#include <rosval.h>

struct PACKSTRUCT linux_dirent64 {
    U64            d_ino;    // Inode number (bisa dummy kalau FS gak support)
    VAL64            d_off;    // Offset to next dirent
    unsigned short d_reclen; // Length of this record
    unsigned char  d_type;   // File type (4=Dir, 8=File, etc)
    char           d_name[1]; // Filename (null-terminated)
};

// Konstanta Tipe File (biar ls bisa kasih warna nanti)
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10