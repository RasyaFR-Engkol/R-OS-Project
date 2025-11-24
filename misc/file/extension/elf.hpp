#pragma once

#include <rosval.h>

#define EI_NIDENT 16
#define PT_LOAD 1
#define ELF_OK 0
#define ELF_ERR_BADMAGIC -1
#define ELF_ERR_UNSUPPORTED -2
#define ELF_ERR_NOMEM -3
#define ELF_ERR_MAPFAIL -4

#define PF_X 0x1  // Execute permission
#define PF_W 0x2  // Write permission
#define PF_R 0x4  // Read permission

#define OUT_ENTRY_ADDRESS


#define DT_NULL 0
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9

#define ELF64_R_TYPE(info) ((uint32_t)((info) & 0xffffffffUL))

#define R_X86_64_RELATIVE 8
#define R_X86_64_64 1

// PT_DYNAMIC (program header type)
#ifndef PT_DYNAMIC
#define PT_DYNAMIC 2
#endif

namespace ELF{
    struct ELF64_EHDR{
        UCHAR8 E_Ident[EI_NIDENT];
        U16 E_Type;
        U16 E_Machine;
        U32 E_Version;
        U64 E_Entry;
        U64 E_Phoff;
        U64 E_Shoff;
        U32 E_Flags;
        U16 E_Ehsize;
        U16 E_Pthentsize;
        U16 E_Pthnum;
        U16 E_Sthentsize;
        U16 E_Sthnum;
        U16 E_Strndx;
    };

    struct ELF64_PHDR{
        U32 P_Type;
        U32 P_Flags;
        U64 P_Offset;
        U64 P_Vaddr;
        U64 P_Paddr;
        U64 P_Filesz;
        U64 P_Memsz;
        U64 P_Align;
    };

    U64 OUT_ENTRY_ADDRESS LoadELF64(VOID *ELFImage,
                                    U64 *TargetCR3,
                                    U64 *ImageBaseOut = nullptr,
                                    U64 *ImageEndOut = nullptr);
}