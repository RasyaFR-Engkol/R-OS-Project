#ifndef ROSVAL_H
#define ROSVAL_H

#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifndef __x86_64__
  #error "Kernel ini cuma buat 64-bit, Bos! Ganti compiler gih."
#endif


/* Basic void / pointer (keep ZEROOBJ as requested) */
typedef void    VOID;
typedef void*   POINTER;
typedef void*   ZEROOBJ; /* user wanted to keep this */
typedef void    NOOBJ;

/* Fixed-width integer aliases (keep original short names for compatibility) */
typedef uint8_t   U8;
typedef uint8_t   U8_T;
typedef int8_t    VAL8;
typedef int8_t    VAL8_T;

typedef uint16_t  U16;
typedef uint16_t  U16_T;
typedef int16_t   VAL16;
typedef int16_t   VAL16_T;

typedef uint32_t  U32;
typedef uint32_t  U32;
typedef uint32_t  U32_T;
typedef int32_t   VAL32;
typedef int32_t   VAL32_T;

typedef uint64_t  U64;
typedef uint64_t  U64_T;
typedef int64_t   VAL64;
typedef int64_t   VAL64_T;

typedef char CHAR8;
typedef unsigned char UCHAR8;

/* Pointer-sized integer types */
typedef uintptr_t UPTR;
typedef intptr_t  SPTR;

/* Size/pointer diff */
typedef size_t    SIZE_T;
typedef ptrdiff_t PTRDIFF_T;

/* Integer but in I not in VAL */
typedef int8_t I8;
typedef int16_t I16;
typedef int32_t I32;
typedef int64_t I64;
typedef int INTN;


/* Boolean */
typedef bool      BOOL;
#ifndef TRUE
#define TRUE      true
#endif
#ifndef FALSE
#define FALSE     false
#endif
typedef bool     FLAGS;
typedef U64 UFLAGS;

/* Untuk flags */
typedef VAL32 FLAGS32;
typedef U32 UFLAGS32;

// Constants 
#define CONSTANTEXPR constexpr
#define CONSTANT const
#define VOLATILE volatile
#define STATIC static
#define INLINE inline

/* Likely/unlikely helpers (and keep RosTrust/RosDoubt semantics) */
#if defined(__GNUC__) || defined(__clang__)
#define RosLikely(x)   __builtin_expect(!!(x), 1)
#define RosUnlikely(x) __builtin_expect(!!(x), 0)
#else
#define RosLikely(x)   (x)
#define RosUnlikely(x) (x)
#endif

#define RosTrust(x)  RosLikely(x)
#define RosDoubt(x)  RosUnlikely(x)

/* Common attribute wrappers */
#if defined(__GNUC__) || defined(__clang__)
#define ROS_UNUSED    __attribute__((unused))
#define ROS_NORETURN  __attribute__((noreturn))
#define ROS_PACKED    __attribute__((packed))
#else
#define ROS_UNUSED
#define ROS_NORETURN
#define ROS_PACKED
#endif

/* Sanity checks when C11 _Static_assert is available */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(U8)  == 1, "U8 must be 1 byte");
_Static_assert(sizeof(U16) == 2, "U16 must be 2 bytes");
_Static_assert(sizeof(U32) == 4, "U32 must be 4 bytes");
_Static_assert(sizeof(UPTR) >= sizeof(void*), "UPTR must hold a pointer");
#endif

#ifdef __cplusplus
#define ABI_C extern "C"
#else
#define ABI_C
#endif

typedef va_list VA_LIST;
#define VA_ARGS(a, b) va_args(a, b)
#define VA_STRT(args, fmt) va_start(args, fmt)
#define VA_END(args) va_end(args)

// MISC Macro
#define UNUSED__ [[maybe_unused]] 
#define __MAYBE_UNUSED UNUSED__
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define OFFSET_OF(type, member) ((SIZE_T)&(((type *)0)->member))
#define PACKSTRUCT __attribute__((packed))

#define NORET __attribute__((noreturn))
#define ONLYASM __attribute__((naked))
#define WEAK __attribute__((weak))
#define ALIGNED(x) __attribute__((aligned(x)))
#define UNREACHABLE __builtin_unreachable()

// Error return value
#define ROS_OK 0
#define ROS_ERR 1
#define ROS_INVALID 2
#define ROS_NOMEM 3
#define ROS_NOTFOUND 4
#define ROS_BUSY 5
#define ROS_UNSUPPORTED 6
#define ROS_IOERROR 7
#define ROS_PERM 8
#define ROS_TIMEOUT 9
#define ROS_EXIST 10
#define ROS_INVALSTATE 11

/*
 * ROSVAL.H ver 2.0
 * 
 * Update: 
 * 1. Implementasi Error value, tidak perlu menebak nebak error ketika
 * terjadi error di syscall atau pusat driver.
 * 
 * 2. Penambahan typedef yang lebih lengkap agar tidak membingungkan
 * untuk developer masa depan
 * 
 * 3. Penambahan macro untuk attribute function dan variable
 * 
 */

// LINUX ERROR RET VALUE
// Based dari linux errno.h, tapi menggunakan format ROS_ERROR PREFIX
// UNTUK GUNAKAN INI, GUNAKAN SAJA
//  return -ROS_ERROR_NO_ENTRY; // Contoh penggunaan
//        ^^^ perhatikan
// 
// Gunakan tanda minus (-) di depan ROS_ERROR_XXX
// karena di linux, nilai error itu negatif
// dan nilai positif itu untuk nilai balik sukses

// LINUX Ret VALUE:
#define ROS_OK 0
#define ROS_ERROR_NO_PERMISSION 1
#define ROS_ERROR_NO_ENTRY 2
#define ROS_ERROR_NO_PROCCESS 3
#define ROS_ERROR_SYSCALL_INTERRUPTED 4
#define ROS_ERROR_INPUT_OUTPUT 5
#define ROS_ERROR_NO_DEV 6
#define ROS_ERROR_EXECVE_TOO_BIG 7
#define ROS_ERROR_EXECVE_INVAL 8
#define ROS_ERROR_BAD_FD 9
#define ROS_ERROR_PROC_NO_CHILD 10
#define ROS_ERROR_TRY_AGAIN_LATER 11
#define ROS_ERROR_OOM 12
#define ROS_ERROR_PERMISSION_DENIED 13
#define ROS_ERROR_FAULTY_ADDRESS 14
#define ROS_ERROR_IS_NOT_BLK 15
#define ROS_ERROR_RESOURCE_BUSY 16
#define ROS_ERROR_FILE_EXIST 17
#define ROS_ERROR_CROSS_DEV 18
#define ROS_ERROR_NO_DRIVE ROS_ERROR_NO_DEV
#define ROS_ERROR_NOT_DIR 20
#define ROS_ERROR_IS_DIR 21
#define ROS_ERROR_INVAL 22  
#define ROS_ERROR_FILE_OVERFLOW 23
#define ROS_ERROR_TOO_MANY_OPEN_FILES 24
#define ROS_ERROR_IS_NOT_TTY 25
#define ROS_ERROR_TEXT_FILE_BUSY 26
#define ROS_ERROR_FILE_TOO_LARGE 27
#define ROS_ERROR_NO_SPACE_LEFT 28
#define ROS_ERROR_FILE_NOT_SEEKABLE 29
#define ROS_ERROR_READ_ONLY_FS 30
#define ROS_ERROR_TOO_MANY_LINKS 31
#define ROS_ERROR_BROKEN_PIPE 32
#define ROS_ERROR_MATH_DOMAIN 33
#define ROS_ERROR_MATH_RANGE 34
#define ROS_ERROR_LOCK_VIOLATION 35
#define ROS_ERROR_FILE_NAME_TOO_LONG 36
#define ROS_ERROR_NO_LOCKS_AVAILABLE 37
#define ROS_ERROR_FUNCTION_NOT_IMPLEMENTED 38
#define ROS_ERROR_DIRECTORY_NOT_EMPTY 39
#define ROS_ERROR_TOO_MANY_SYMBOLIC_LINKS 40

#define ROS_ERROR_WOULD_BLOCK              41 // Sama kayak EAGAIN, resource belum siap

// Error nomor 42 - 44: Spesifik untuk Networking & Quota
#define ROS_ERROR_NO_MESSAGE               42 // Gak ada pesan dari tipe yang diinginkan di antrian
#define ROS_ERROR_IDENTIFIER_REMOVED       43 // ID IPC (Inter-process) dihapus pas lagi dipake
#define ROS_ERROR_CHANNEL_OUT_OF_RANGE     44 // Nomor channel hardware/network di luar batas

// Error nomor 45 - 50: Area Networking & Sync
#define ROS_ERROR_LEVEL2_NOT_SYNC          45 // Sinkronisasi hardware level 2 gagal
#define ROS_ERROR_LEVEL3_HALTED            46 // Hardware level 3 berhenti (Halt)
#define ROS_ERROR_LEVEL3_RESET             47 // Hardware level 3 di-reset
#define ROS_ERROR_LINK_NUMBER_OUT_OF_RANGE 48 // Nomor link hardware gak valid
#define ROS_ERROR_PROTOCOL_DRIVER_NOT_ATTACHED 49 // Driver protokol (TCP/IP?) gak nempel
#define ROS_ERROR_NO_CSI_STRUCTURE         50 // Gak ada struktur CSI (Internal Hardware)

// Error nomor 51 - 55: Network & File System spesifik
#define ROS_ERROR_LEVEL2_HALTED            51 // Hardware level 2 berhenti
#define ROS_ERROR_INVALID_EXCHANGE         52 // Pertukaran data (exchange) gak valid
#define ROS_ERROR_INVALID_REQUEST_DESCRIPTOR 53 // Request descriptor data rusak/salah
#define ROS_ERROR_EXCHANGE_FULL            54 // Slot pertukaran data penuh
#define ROS_ERROR_NO_ANODE                 55 // Gak ada "Anode" (Mirip Inode tapi buat sistem file tertentu)

#define ROS_ERROR_INVALID_SLOT             56 // Slot hardware/index gak valid
#define ROS_ERROR_BAD_FONT_FORMAT          57 // Format file font rusak (penting buat GUI nanti!)
#define ROS_ERROR_NOT_A_STREAM             60 // File bukan tipe "Stream" (buat sistem I/O)
#define ROS_ERROR_NO_DATA_AVAILABLE        61 // Gak ada data yang bisa dibaca (No data available)
#define ROS_ERROR_TIMER_EXPIRED            62 // Waktu tunggu (timer) udah abis
#define ROS_ERROR_OUT_OF_STREAMS_RESOURCES 63 // Resource buat streaming data abis
#define ROS_ERROR_MACHINE_NOT_ON_NETWORK   64 // Komputer gak konek ke jaringan
#define ROS_ERROR_PACKAGE_NOT_INSTALLED    65 // Paket/Modul yang diminta gak terpasang
#define ROS_ERROR_OBJECT_IS_REMOTE         66 // File/Object ada di komputer lain (Remote)
#define ROS_ERROR_LINK_HAS_BEEN_SEVERED    67 // Koneksi jaringan/Virtual terputus
#define ROS_ERROR_ADVERTISE_ERROR          68 // Error saat mencoba membagikan (advertise) resource
#define ROS_ERROR_SRM_MOUNT_ERROR          69 // Error saat mounting resource jaringan
#define ROS_ERROR_COMMUNICATION_ERROR      70 // Terjadi error komunikasi saat pengiriman data

#define ROS_ERROR_PROTOCOL_ERROR           71 // Error pada protokol komunikasi
#define ROS_ERROR_MULTIHOP_ATTEMPTED       72 // Mencoba akses file melewati terlalu banyak hop jaringan
#define ROS_ERROR_RFS_SPECIFIC_ERROR       73 // Error spesifik pada sistem file remote (RFS)
#define ROS_ERROR_BAD_MESSAGE              74 // Pesan data korup atau tidak valid
#define ROS_ERROR_VALUE_TOO_LARGE          75 // Nilai terlalu besar untuk tipe data (Overflow)
#define ROS_ERROR_NAME_NOT_UNIQUE          76 // Nama di jaringan tidak unik (bentrok)
#define ROS_ERROR_FILE_DESCRIPTOR_STATE    77 // FD (File Descriptor) dalam kondisi tidak valid
#define ROS_ERROR_REMOTE_ADDRESS_CHANGED   78 // Alamat remote berubah tiba-tiba
#define ROS_ERROR_CANNOT_ACCESS_LIB        79 // Tidak bisa akses shared library (.DLL / .SO) yang dibutuhin
#define ROS_ERROR_LIB_CORRUPTED            80 // File library (.DLL) rusak atau korup
#define ROS_ERROR_LIB_SECTION_CORRUPTED    81 // Bagian (.section) di dalam library rusak
#define ROS_ERROR_TOO_MANY_LIBS            82 // Terlalu banyak shared library yang di-load
#define ROS_ERROR_CANNOT_EXEC_LIB_DIRECTLY 83 // Mencoba jalanin library (.DLL) secara langsung
#define ROS_ERROR_ILLEGAL_BYTE_SEQUENCE    84 // Urutan byte karakter ilegal (Error encoding/Unicode)
#define ROS_ERROR_INTERRUPTED_RESTART      85 // Syscall ke-interrupt dan harus di-restart
#define ROS_ERROR_STREAMS_PIPE_ERROR       86 // Error pada aliran pipa (Streams pipe error)
#define ROS_ERROR_TOO_MANY_USERS           87 // Jumlah user di sistem sudah maksimal
#define ROS_ERROR_SOCKET_NOT_A_SOCKET      88 // Operasi socket dilakukan pada file biasa
#define ROS_ERROR_DESTINATION_ADDR_REQ     89 // Alamat tujuan dibutuhkan (Destination address required)
#define ROS_ERROR_MESSAGE_TOO_LONG         90 // Pesan terlalu panjang (Message too long)
#define ROS_ERROR_PROTOCOL_WRONG_TYPE      91 // Tipe protokol salah untuk socket ini
#define ROS_ERROR_PROTOCOL_NOT_AVAILABLE   92 // Protokol tidak tersedia di kernel
#define ROS_ERROR_PROTOCOL_NOT_SUPPORTED   93 // Protokol tidak didukung oleh sistem
#define ROS_ERROR_SOCKET_TYPE_NOT_SUPP     94 // Tipe socket tidak didukung
#define ROS_ERROR_OP_NOT_SUPPORTED         95 // Operasi tidak didukung (Operation not supported)
#define ROS_ERROR_PROTOCOL_FAMILY_NOT_SUPP 96 // Family protokol tidak didukung
#define ROS_ERROR_ADDR_FAMILY_NOT_SUPP     97 // Address family tidak didukung oleh protokol
#define ROS_ERROR_ADDR_IN_USE              98 // Alamat (IP/Port) sudah dipakai proses lain
#define ROS_ERROR_ADDR_NOT_AVAILABLE       99 // Alamat yang diminta tidak tersedia
#define ROS_ERROR_NETWORK_DOWN             100 // Jaringan mati total (Network is down)
#define ROS_ERROR_NETWORK_UNREACHABLE      101 // Jaringan gak bisa dijangkau (No route)
#define ROS_ERROR_NETWORK_RESET            102 // Koneksi diputus oleh jaringan (Reset)
#define ROS_ERROR_CONNECTION_ABORTED       103 // Koneksi dibatalkan oleh software lu sendiri
#define ROS_ERROR_CONNECTION_RESET         104 // Koneksi di-reset oleh pihak sana (Peer)
#define ROS_ERROR_NO_BUFFER_SPACE          105 // Memori buffer buat network abis (Buffer full)
#define ROS_ERROR_SOCKET_IS_CONNECTED      106 // Socket udah konek, gak bisa konek lagi
#define ROS_ERROR_SOCKET_NOT_CONNECTED     107 // Socket belum konek pas mau kirim data
#define ROS_ERROR_SHUTDOWN_SEND_CANNOT     108 // Gak bisa ngirim karena socket udah di-shutdown
#define ROS_ERROR_TOO_MANY_REFERENCES      109 // Terlalu banyak referensi object (Object leak)
#define ROS_ERROR_CONNECTION_TIMED_OUT     110 // Kelamaan nunggu balesan (Timeout)
#define ROS_ERROR_CONNECTION_REFUSED       111 // Koneksi ditolak oleh target (Refused)
#define ROS_ERROR_HOST_IS_DOWN             112 // Komputer target mati
#define ROS_ERROR_NO_ROUTE_TO_HOST         113 // Gak ada jalur (route) ke komputer target
#define ROS_ERROR_ALREADY_IN_PROGRESS      114 // Operasi lagi jalan, jangan dipanggil lagi
#define ROS_ERROR_OPERATION_IN_PROGRESS    115 // Operasi lagi diproses (Non-blocking)
#define ROS_ERROR_STALE_FILE_HANDLE        116 // Handle file udah kadaluwarsa/basi
#define ROS_ERROR_STRUCTURE_NEEDS_CLEANING 117 // Struktur data (FS) rusak, butuh perbaikan
#define ROS_ERROR_NOT_A_XENIX_NAMED_TYPE   118 // Bukan tipe XENIX (Error jadul tapi tetep ada)
#define ROS_ERROR_NO_XENIX_SEMAPHORES      119 // Gak ada semaphore XENIX tersedia
#define ROS_ERROR_IS_NAMED_FILE_TYPE       120 // Ini adalah tipe file bernama (Named file)
#define ROS_ERROR_REMOTE_IO_ERROR          121 // Terjadi error I/O di komputer seberang (Remote)
#define ROS_ERROR_DISK_QUOTA_EXCEEDED      122 // Jatah (Quota) penyimpanan user udah abis
#define ROS_ERROR_NO_MEDIUM                123 // Gak ada kaset/CD/Disk di dalem drivenya
#define ROS_ERROR_WRONG_MEDIUM_TYPE        124 // Tipe disk yang dimasukin salah
#define ROS_ERROR_OPERATION_CANCELED       125 // Operasi dibatalkan secara sengaja
#define ROS_ERROR_KEY_NOT_AVAILABLE        126 // Kunci keamanan (Security Key) gak ada
#define ROS_ERROR_KEY_EXPIRED              127 // Kunci keamanan udah kadaluwarsa
#define ROS_ERROR_KEY_REVOKED              128 // Kunci keamanan udah dicabut
#define ROS_ERROR_KEY_REJECTED             129 // Kunci keamanan ditolak oleh sistem
#define ROS_ERROR_OWNER_DIED               130 // Pemilik mutex/lock udah mati (Deadlock risk)
#define ROS_ERROR_STATE_NOT_RECOVERABLE    131 // Kondisi error parah, gak bisa balik normal
#define ROS_ERROR_ERFKILL                  132 // Operasi gagal karena "Radio Kill Switch" (Flight mode)
#define ROS_ERROR_HW_POISON                133 // Memori/Hardware "beracun" (Rusak fisik parah)

// TIPE DATA TERBARU ROSVAL.H

typedef U8     BYTE;
typedef U16    WORD;
typedef U32    DWORD;
typedef U64    QWORD;

typedef I8    INT8;
typedef I16   INT16;
typedef I32   INT32;
typedef I64   INT64;

typedef I32 RESULTFUNC; // Untuk nilai balik fungsi umum
typedef VOID VOIDFUNC; // Untuk fungsi yang gak balik apa apa
typedef BOOL BOOLFUNC; // Untuk fungsi yang balik boolean
typedef POINTER POINTERFUNC; // Untuk fungsi yang balik pointer
typedef VOIDFUNC NORESULTFUNC;

typedef POINTER RHANDLE, EVENTHANDLE, DEVICEHANDLE, CHARHANDLE, OTHERDEVICEHANDLE; // General handle type mirip WINDOWS
typedef POINTER EVENTHANDLE; // Handle untuk event/signal

typedef uintptr_t RWPARAM; // General read/write parameter type
typedef intptr_t RLPARAM;  // General long parameter type

// PEWARNAAN (KHUSUS UNTUK KONSOLE / TERMINAL)
typedef U32 RCOLORARGB;
// end

// KHUSUS UNSIGNED & POINTER
typedef BYTE* P_BYTE;
typedef WORD* P_WORD;
typedef DWORD* P_DWORD;
typedef QWORD* P_QWORD;

// KHUSUS non unsigned & pointer
typedef INT8* P_I8;
typedef INT16* P_I16;
typedef INT32* P_I32;
typedef INT64* P_I64;
typedef P_I8 P_INT8;
typedef P_I16 P_INT16;
typedef P_I32 P_INT32;
typedef P_I64 P_INT64;

// RESERVED PROC
typedef int PID_T, FD_T;

// INPUT TEXT
typedef CHAR8 ANSI_STRING;
typedef CONSTANT CHAR8 CANSI_STRING, *CPANSI_STRING;
typedef wchar_t UNICODE_STRING;

// fKHUSUS
// SIlent warning to my compiler itself
#define ARGIN
#define ARGOUT __MAYBE_UNUSED
#define ARGINOUT

struct FBRect {
    U32 x;
    U32 y;
    U32 w;
    U32 h;
};

#endif