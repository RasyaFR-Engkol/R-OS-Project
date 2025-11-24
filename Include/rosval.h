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

#endif