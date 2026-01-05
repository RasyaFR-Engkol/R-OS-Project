#include <rosval.h>
#include <rossys.hpp>
#include "elf.hpp"
#include <mm.hpp>
#include <string.hpp>
#include <stdint.h>
#define PRINTK_MODULE_NAME "ELFEXT"
#include <logging.hpp>

namespace {
constexpr U64 USER_PIE_BASE = 0x400000ULL;
constexpr U64 USER_PIE_GUARD = 0x200000ULL; // leave 2 MiB gap between images

static U64 G_UserPieCursor = USER_PIE_BASE;

struct CachedUserPage {
    U64 pageBase;
    U8 *pagePtr;
};

static CachedUserPage G_ReadCache{0, nullptr};
static CachedUserPage G_WriteCache{0, nullptr};

static inline U64 AlignUp(U64 value, U64 alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

struct Elf64Dyn {
    int64_t d_tag;
    union { U64 d_val; U64 d_ptr; } d_un;
};

struct Elf64Rela {
    U64 r_offset;
    U64 r_info;
    int64_t r_addend;
};

static U64 *G_TargetPML4 = nullptr;
static bool TranslateUserPage(U64 *pml4, U64 virtAddr, U64 *physPageOut);

static inline void ResetUserAccessCaches() {
    G_ReadCache.pagePtr = nullptr;
    G_WriteCache.pagePtr = nullptr;
}

static bool EnsureCachedPage(U64 virtAddr, CachedUserPage &cache) {
    if (!G_TargetPML4) return false;
    U64 pageBase = virtAddr & ~(PAGE_SIZE - 1ULL);
    if (cache.pagePtr && cache.pageBase == pageBase) {
        return true;
    }

    U64 physPage;
    if (!TranslateUserPage(G_TargetPML4, pageBase, &physPage)) {
        return false;
    }

    cache.pageBase = pageBase;
    cache.pagePtr = reinterpret_cast<U8*>(HHDM_PhysToVirt(physPage));
    return cache.pagePtr != nullptr;
}

static U64 ReserveDynamicImageBase(U64 spanBytes) {
    spanBytes = AlignUp(spanBytes ? spanBytes : PAGE_SIZE, PAGE_SIZE);
    U64 base = AlignUp(G_UserPieCursor, PAGE_SIZE);
    U64 next = AlignUp(base + spanBytes + USER_PIE_GUARD, PAGE_SIZE);
    G_UserPieCursor = next;
    return base;
}

static inline U64 RelocateAddress(U64 original, U64 targetBase, U64 elfBase) {
    if (targetBase == elfBase) return original;
    return targetBase + (original - elfBase);
}

static bool TranslateUserPage(U64 *pml4, U64 virtAddr, U64 *physPageOut) {
    if (!pml4 || !physPageOut) return false;

    U64 pml4e = pml4[(virtAddr >> 39) & 0x1FFu];
    if (!(pml4e & PAGE_PRESENT)) return false;
    U64 *pdpt = HHDM_PhysToVirt(pml4e & PAGE_ADDR_MASK);

    U64 pdpte = pdpt[(virtAddr >> 30) & 0x1FFu];
    if (!(pdpte & PAGE_PRESENT) || (pdpte & PAGE_PS)) return false;
    U64 *pd = HHDM_PhysToVirt(pdpte & PAGE_ADDR_MASK);

    U64 pde = pd[(virtAddr >> 21) & 0x1FFu];
    if (!(pde & PAGE_PRESENT) || (pde & PAGE_PS)) return false;
    U64 *pt = HHDM_PhysToVirt(pde & PAGE_ADDR_MASK);

    U64 pte = pt[(virtAddr >> 12) & 0x1FFu];
    if (!(pte & PAGE_PRESENT)) return false;

    *physPageOut = pte & PAGE_ADDR_MASK;
    return true;
}

static bool ReadUser64(U64 virtAddr, U64 *value) {
    if (!G_TargetPML4 || !value) return false;
    if (!EnsureCachedPage(virtAddr, G_ReadCache)) return false;
    SIZE_T offset = (SIZE_T)(virtAddr - G_ReadCache.pageBase);
    *value = *reinterpret_cast<U64*>(G_ReadCache.pagePtr + offset);
    return true;
}

static bool WriteUser64(U64 virtAddr, U64 value) {
    if (!G_TargetPML4) return false;
    if (!EnsureCachedPage(virtAddr, G_WriteCache)) return false;
    SIZE_T offset = (SIZE_T)(virtAddr - G_WriteCache.pageBase);
    *reinterpret_cast<U64*>(G_WriteCache.pagePtr + offset) = value;
    Arch::Invlpg((UPTR)virtAddr);
    // Keep read cache coherent if both touch the same page
    if (G_ReadCache.pagePtr && G_ReadCache.pageBase == G_WriteCache.pageBase) {
        G_ReadCache = G_WriteCache;
    }
    return true;
}

static bool ProcessDynamicSection(const ELF::ELF64_PHDR *phDynamic,
                                  U64 targetBase,
                                  U64 elfBase) {
    if (!phDynamic) return true;

    U64 dynBase = RelocateAddress(phDynamic->P_Vaddr, targetBase, elfBase);
    U64 dynSize = phDynamic->P_Filesz;
    U64 offset = 0;
    U64 relaAddr = 0;
    U64 relaSize = 0;
    U64 relaEnt = sizeof(Elf64Rela);

    while (offset + sizeof(Elf64Dyn) <= dynSize) {
        U64 entryVA = dynBase + offset;
        U64 tagRaw;
        U64 valRaw;
        if (!ReadUser64(entryVA, &tagRaw) || !ReadUser64(entryVA + 8, &valRaw)) {
            Printk::Write(Printk::Level::LOG_ERR,
                          "ELF: Failed to read PT_DYNAMIC entry at 0x%016llx\n",
                          (unsigned long long)entryVA);
            return false;
        }

        int64_t tag = (int64_t)tagRaw;
        if (tag == DT_NULL) break;
        if (tag == DT_RELA) {
            relaAddr = RelocateAddress(valRaw, targetBase, elfBase);
        } else if (tag == DT_RELASZ) {
            relaSize = valRaw;
        } else if (tag == DT_RELAENT) {
            relaEnt = valRaw ? valRaw : sizeof(Elf64Rela);
        }

        offset += sizeof(Elf64Dyn);
    }

    if (!relaAddr || !relaSize || !relaEnt) return true;

    U64 count = relaSize / relaEnt;
    Printk::Write(Printk::Level::LOG_DEBUG,
                  "ELF: Applying %llu RELA entries\n",
                  (unsigned long long)count);

    for (U64 i = 0; i < count; ++i) {
        U64 entryVA = relaAddr + i * relaEnt;
        U64 rOffset;
        U64 rInfo;
        U64 rAddendRaw;
        if (!ReadUser64(entryVA + 0, &rOffset) ||
            !ReadUser64(entryVA + 8, &rInfo) ||
            !ReadUser64(entryVA + 16, &rAddendRaw)) {
            Printk::Write(Printk::Level::LOG_ERR,
                          "ELF: Failed to read RELA entry at 0x%016llx\n",
                          (unsigned long long)entryVA);
            return false;
        }

        U32 type = ELF64_R_TYPE(rInfo);
        U64 targetVA = RelocateAddress(rOffset, targetBase, elfBase);
        int64_t addend = (int64_t)rAddendRaw;
        U64 newValue = 0;

        if (type == R_X86_64_RELATIVE) {
            newValue = targetBase + (U64)addend;
        } else if (type == R_X86_64_64) {
            newValue = targetBase + (U64)addend;
        } else {
            Printk::Write(Printk::Level::LOG_WARNING,
                          "ELF: Unhandled RELA type %u at index %llu\n",
                          type, (unsigned long long)i);
            continue;
        }

        if (!WriteUser64(targetVA, newValue)) {
            Printk::Write(Printk::Level::LOG_ERR,
                          "ELF: Failed to write relocation target 0x%016llx\n",
                          (unsigned long long)targetVA);
            return false;
        }
    }

    return true;
}
}

namespace ELF{
    U64 LoadELF64(VOID *ELFImage,
                  U64 *TargetCR3,
                  U64 *ImageBaseOut,
                  U64 *ImageEndOut){
        if (!ELFImage || !TargetCR3) {
            Printk::Write(Printk::Level::LOG_ERR, "ELF: Invalid parameters to LoadELF64\n");
            return ELF_ERR_UNSUPPORTED;
        }

        G_TargetPML4 = TargetCR3;
        ResetUserAccessCaches();

        auto *EHDR = (ELF64_EHDR *)ELFImage;
        if(EHDR->E_Ident[0] != 0x7F || EHDR->E_Ident[1] != 'E' ||
           EHDR->E_Ident[2] != 'L' || EHDR->E_Ident[3] != 'F'){
            Printk::Write(Printk::Level::LOG_WARNING, "ELF: Bad magic number!\n");
            return ELF_ERR_BADMAGIC;
        }

        auto *PHDR = reinterpret_cast<ELF64_PHDR *>((UPTR)ELFImage + EHDR->E_Phoff);

        U64 ElfBaseAddr = (U64)-1;
        U64 MaxSegmentEnd = 0;
        bool FoundLoad = false;
        for(U16 i = 0; i < EHDR->E_Pthnum; i++){
            if(PHDR[i].P_Type != PT_LOAD) continue;
            FoundLoad = true;
            if(PHDR[i].P_Vaddr < ElfBaseAddr){
                ElfBaseAddr = PHDR[i].P_Vaddr;
            }
            U64 segEnd = PHDR[i].P_Vaddr + PHDR[i].P_Memsz;
            if (segEnd > MaxSegmentEnd) {
                MaxSegmentEnd = segEnd;
            }
        }

        if(!FoundLoad){
            Printk::Write(Printk::Level::LOG_ERR, "ELF: No loadable segments found!\n");
            return ELF_ERR_UNSUPPORTED;
        }

        U64 ImageSpan = (MaxSegmentEnd > ElfBaseAddr) ? (MaxSegmentEnd - ElfBaseAddr) : PAGE_SIZE;
        if (ImageSpan == 0) {
            ImageSpan = PAGE_SIZE;
        }
        U64 ImageSpanAligned = AlignUp(ImageSpan, PAGE_SIZE);

        #ifndef ET_EXEC
        #define ET_EXEC 2
        #endif
        #ifndef ET_DYN
        #define ET_DYN 3
        #endif

        U64 TargetBaseAddrMode = (EHDR->E_Type == ET_DYN)
                                 ? ReserveDynamicImageBase(ImageSpanAligned)
                                 : ElfBaseAddr;
        U64 ImageRangeEnd = TargetBaseAddrMode + ImageSpanAligned;
        const U8 *ElfBytes = (const U8 *)ELFImage;

        for(U32 i = 0; i < EHDR->E_Pthnum; i++){
            if(PHDR[i].P_Type != PT_LOAD) continue;

            U64 RelocatedVaddr = (TargetBaseAddrMode == ElfBaseAddr)
                                 ? PHDR[i].P_Vaddr
                                 : TargetBaseAddrMode + (PHDR[i].P_Vaddr - ElfBaseAddr);

            U64 SegmentSize = PHDR[i].P_Memsz;
            U64 FileSize = PHDR[i].P_Filesz;
            U64 OffsetInFile = PHDR[i].P_Offset;

            U64 VaddrPage = RelocatedVaddr & ~(PAGE_SIZE - 1);

            // Security: pastikan alokasi tidak melebihi HHDM
            if (VaddrPage >= 0xFFFF800000000000ULL || VaddrPage >= HHDM_BASE) { // Ganti sesuai batas Kernel lo (Higher Half)
                Printk::Write(Printk::Level::LOG_ERR, 
                    "ELF SECURITY: User program tried to map kernel memory at %llx!\n", VaddrPage);
                return ELF_ERR_UNSUPPORTED;
            }

            U64 SizePage = ((RelocatedVaddr + SegmentSize + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)) - VaddrPage;
            if (!SizePage) continue;

            //Printk::Write(Printk::Level::LOG_DEBUG,
            //              "ELF: Segment %u flags=0x%X vaddr=0x%016llx memsz=0x%llx\n",
            //              i, PHDR[i].P_Flags,
            //              (unsigned long long)RelocatedVaddr,
            //              (unsigned long long)SegmentSize);

            for(U64 Off = 0; Off < SizePage; Off += PAGE_SIZE){
                U64 CurrentVaddrUser = VaddrPage + Off;
                UPTR PhysPage = PageAlloc::PhysicalAllocPages(1);
                if(!PhysPage){
                    Printk::Write(Printk::Level::LOG_ERR,
                                  "ELF: Out of memory while loading segment %u\n", i);
                    return ELF_ERR_NOMEM;
                }

                auto *TempKernelPtr = reinterpret_cast<U8*>(HHDM_PhysToVirt(PhysPage));
                if(!TempKernelPtr){
                    Printk::Write(Printk::Level::LOG_ERR,
                                  "ELF: Failed to get HHDM pointer for phys 0x%016llx\n",
                                  (unsigned long long)PhysPage);
                    PageAlloc::PhysicalFreePages(PhysPage, 1);
                    return ELF_ERR_MAPFAIL;
                }

                String::Memset(TempKernelPtr, 0, PAGE_SIZE);

                U64 CopyStart = (CurrentVaddrUser > RelocatedVaddr) ? CurrentVaddrUser : RelocatedVaddr;
                U64 SegmentEnd = RelocatedVaddr + FileSize;
                U64 CopyEnd = ((CurrentVaddrUser + PAGE_SIZE) < SegmentEnd)
                               ? (CurrentVaddrUser + PAGE_SIZE)
                               : SegmentEnd;

                if(CopyStart < CopyEnd){
                    U64 CopySize = CopyEnd - CopyStart;
                    U64 SrcOffset = OffsetInFile + (CopyStart - RelocatedVaddr);
                    U64 DstOffset = CopyStart - CurrentVaddrUser;
                    String::Memcpy(TempKernelPtr + DstOffset, ElfBytes + SrcOffset, CopySize);
                }

                U64 PageFlags = PAGE_PRESENT | PAGE_USER;
                if(PHDR[i].P_Flags & PF_W) PageFlags |= PAGE_RW;
                if(!(PHDR[i].P_Flags & PF_X)) PageFlags |= PAGE_NX;

                if(!PageAlloc::MapPages(TargetCR3, PhysPage, CurrentVaddrUser, 1, PageFlags)){
                    Printk::Write(Printk::Level::LOG_ERR,
                                  "ELF: Failed to map page at vaddr 0x%016llx\n",
                                  (unsigned long long)CurrentVaddrUser);
                    PageAlloc::PhysicalFreePages(PhysPage, 1);
                    return ELF_ERR_MAPFAIL;
                }
            }
        }

        ELF64_PHDR *PHDynamic = nullptr;
        for(U32 i = 0; i < EHDR->E_Pthnum; i++){
            if(PHDR[i].P_Type == PT_DYNAMIC) {PHDynamic = &PHDR[i]; break;}
        }

        if(!ProcessDynamicSection(PHDynamic, TargetBaseAddrMode, ElfBaseAddr)){
            return ELF_ERR_UNSUPPORTED;
        }

        U64 relocatedEntry;
        if (TargetBaseAddrMode == ElfBaseAddr) {
            relocatedEntry = EHDR->E_Entry;
        } else {
            relocatedEntry = TargetBaseAddrMode + (EHDR->E_Entry - ElfBaseAddr);
        }

        if (ImageBaseOut) *ImageBaseOut = TargetBaseAddrMode;
        if (ImageEndOut) *ImageEndOut = ImageRangeEnd;
        //Printk::Write(Printk::Level::LOG_DEBUG,
        //              "ELF: Loaded image base=0x%016llx end=0x%016llx entry=0x%016llx\n",
        //              (unsigned long long)TargetBaseAddrMode,
        //              (unsigned long long)ImageRangeEnd,
        //              (unsigned long long)relocatedEntry);
        return relocatedEntry;
    }
}