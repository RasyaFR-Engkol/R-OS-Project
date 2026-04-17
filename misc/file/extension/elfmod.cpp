#include <rosval.h>
#include <rossys.hpp>
#include "elf.hpp"
#include <mm.hpp>
#include <string.hpp>
#include <stdint.h>
#define PRINTK_MODULE_NAME "KMOD"
#include <logging.hpp>
#include <../kernel/mod/module_manager.hpp>

namespace {
// Pindahkan base ke area Higher Half khusus Kernel Modules (contoh: -1GB dari top)
constexpr U64 KERNEL_MODULE_BASE = 0xFFFFFFFFC0000000ULL;
constexpr U64 KERNEL_MODULE_GUARD = 0x200000ULL; // gap 2 MiB antar modul

static U64 G_ModuleCursor = KERNEL_MODULE_BASE;

struct CachedModulePage {
    U64 pageBase;
    U8 *pagePtr;
};

static CachedModulePage G_ReadCache{0, nullptr};
static CachedModulePage G_WriteCache{0, nullptr};

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
static bool TranslateModulePage(U64 *pml4, U64 virtAddr, U64 *physPageOut);

static inline void ResetModuleAccessCaches() {
    G_ReadCache.pagePtr = nullptr;
    G_WriteCache.pagePtr = nullptr;
}

static bool EnsureCachedPage(U64 virtAddr, CachedModulePage &cache) {
    if (!G_TargetPML4) return false;
    U64 pageBase = virtAddr & ~(PAGE_SIZE - 1ULL);
    if (cache.pagePtr && cache.pageBase == pageBase) {
        return true;
    }

    U64 physPage;
    if (!TranslateModulePage(G_TargetPML4, pageBase, &physPage)) {
        return false;
    }

    cache.pageBase = pageBase;
    cache.pagePtr = reinterpret_cast<U8*>(HHDM_PhysToVirt(physPage));
    return cache.pagePtr != nullptr;
}

static U64 ReserveDynamicImageBase(U64 spanBytes) {
    spanBytes = AlignUp(spanBytes ? spanBytes : PAGE_SIZE, PAGE_SIZE);
    U64 base = AlignUp(G_ModuleCursor, PAGE_SIZE);
    U64 next = AlignUp(base + spanBytes + KERNEL_MODULE_GUARD, PAGE_SIZE);
    G_ModuleCursor = next;
    return base;
}

static inline U64 RelocateAddress(U64 original, U64 targetBase, U64 elfBase) {
    if (targetBase == elfBase) return original;
    return targetBase + (original - elfBase);
}

static bool TranslateModulePage(U64 *pml4, U64 virtAddr, U64 *physPageOut) {
    if (!pml4 || !physPageOut) return false;

    U64 pml4e = pml4[(virtAddr >> 39) & 0x1FFu];
    if (!(pml4e & PAGE_PRESENT)) return false;
    U64 *pdpt = reinterpret_cast<U64*>(HHDM_PhysToVirt(pml4e & PAGE_ADDR_MASK));

    U64 pdpte = pdpt[(virtAddr >> 30) & 0x1FFu];
    if (!(pdpte & PAGE_PRESENT) || (pdpte & PAGE_PS)) return false;
    U64 *pd = reinterpret_cast<U64*>(HHDM_PhysToVirt(pdpte & PAGE_ADDR_MASK));

    U64 pde = pd[(virtAddr >> 21) & 0x1FFu];
    if (!(pde & PAGE_PRESENT) || (pde & PAGE_PS)) return false;
    U64 *pt = reinterpret_cast<U64*>(HHDM_PhysToVirt(pde & PAGE_ADDR_MASK));

    U64 pte = pt[(virtAddr >> 12) & 0x1FFu];
    if (!(pte & PAGE_PRESENT)) return false;

    *physPageOut = pte & PAGE_ADDR_MASK;
    return true;
}

static bool ReadModule64(U64 virtAddr, U64 *value) {
    if (!G_TargetPML4 || !value) return false;
    if (!EnsureCachedPage(virtAddr, G_ReadCache)) return false;
    SIZE_T offset = (SIZE_T)(virtAddr - G_ReadCache.pageBase);
    *value = *reinterpret_cast<U64*>(G_ReadCache.pagePtr + offset);
    return true;
}

static bool WriteModule64(U64 virtAddr, U64 value) {
    if (!G_TargetPML4) return false;
    if (!EnsureCachedPage(virtAddr, G_WriteCache)) return false;
    SIZE_T offset = (SIZE_T)(virtAddr - G_WriteCache.pageBase);
    *reinterpret_cast<U64*>(G_WriteCache.pagePtr + offset) = value;
    Arch::Invlpg((UPTR)virtAddr);
    
    if (G_ReadCache.pagePtr && G_ReadCache.pageBase == G_WriteCache.pageBase) {
        G_ReadCache = G_WriteCache;
    }
    return true;
}

static bool ReadModuleString(U64 VirtAddr, CHAR8* outBuf, SIZE_T maxLen){
    if (!G_TargetPML4 || !outBuf || maxLen == 0) return false;

    for (SIZE_T i = 0; i < maxLen; i++) {
        if (!EnsureCachedPage(VirtAddr + i, G_ReadCache)) return false;
        SIZE_T offset = (SIZE_T)((VirtAddr + i) - G_ReadCache.pageBase);
        outBuf[i] = reinterpret_cast<char*>(G_ReadCache.pagePtr)[offset];
        
        // Berhenti kalau ketemu null-terminator
        if (outBuf[i] == '\0') return true;
    }
    outBuf[maxLen - 1] = '\0'; // Safety ujungnya
    return true;
}

static bool ProcessDynamicSection(const ELF::ELF64_PHDR *phDynamic,
                                  U64 targetBase,
                                  U64 elfBase) {
    if (!phDynamic){
        Printk::Write(Printk::Level::LOG_WARNING, "KMOD: No PT_DYNAMIC segment found, skipping relocation\n");
        return true; 
    }

    U64 dynBase = RelocateAddress(phDynamic->P_Vaddr, targetBase, elfBase);
    U64 dynSize = phDynamic->P_Filesz;
    U64 offset = 0;
    
    // Variabel buat dua jenis relokasi
    U64 relaAddr = 0, relaSize = 0, relaEnt = sizeof(Elf64Rela);
    U64 jmpRelAddr = 0, jmpRelSize = 0; // Tambahan buat PLT (Function Relocations)
    U64 symtabAddr = 0, strtabAddr = 0;

    // 1. Parsing PT_DYNAMIC buat narik semua alamat penting
    while (offset + sizeof(Elf64Dyn) <= dynSize) {
        U64 entryVA = dynBase + offset;
        U64 tagRaw, valRaw;
        if (!ReadModule64(entryVA, &tagRaw) || !ReadModule64(entryVA + 8, &valRaw)) return false;

        int64_t tag = (int64_t)tagRaw;
        if (tag == DT_NULL) break;
        
        if (tag == DT_RELA) {
            relaAddr = RelocateAddress(valRaw, targetBase, elfBase);
        } else if (tag == DT_RELASZ) {
            relaSize = valRaw;
        } else if (tag == DT_RELAENT) {
            relaEnt = valRaw ? valRaw : sizeof(Elf64Rela);
        } else if (tag == DT_JMPREL) {
            jmpRelAddr = RelocateAddress(valRaw, targetBase, elfBase); // Tabel Relokasi Fungsi
        } else if (tag == DT_PLTRELSZ) {
            jmpRelSize = valRaw; // Ukuran Tabel Relokasi Fungsi
        } else if (tag == DT_SYMTAB) {
            symtabAddr = RelocateAddress(valRaw, targetBase, elfBase);
        } else if (tag == DT_STRTAB) {
            strtabAddr = RelocateAddress(valRaw, targetBase, elfBase);
        }

        offset += sizeof(Elf64Dyn);
    }

    // 2. Bikin Helper/Lambda buat nge-apply relokasi biar kodenya nggak ngulang
    auto ApplyRelocations = [&](U64 rAddr, U64 rSize, const char* tableName) -> bool {
        if (!rAddr || !rSize) return true; // Skip kalau tabelnya kosong
        
        U64 count = rSize / relaEnt;
        Printk::Write(Printk::Level::LOG_INFO, "KMOD: Applying %llu %s entries\n", count, tableName);

        for (U64 i = 0; i < count; ++i) {
            U64 entryVA = rAddr + i * relaEnt;
            U64 rOffset, rInfo, rAddendRaw;
            if (!ReadModule64(entryVA + 0, &rOffset) ||
                !ReadModule64(entryVA + 8, &rInfo) ||
                !ReadModule64(entryVA + 16, &rAddendRaw)) {
                return false;
            }

            U32 type = ELF64_R_TYPE(rInfo);
            U32 symIdx = ELF64_R_SYM(rInfo); 
            U64 targetVA = RelocateAddress(rOffset, targetBase, elfBase);
            int64_t addend = (int64_t)rAddendRaw;
            
            U64 symValue = 0;

            // Resolve External Symbol dari Kernel
            if (symIdx != 0 && symtabAddr != 0 && strtabAddr != 0) {
                U64 symEntryVA = symtabAddr + (symIdx * sizeof(ELF::Elf64_Sym));
                U64 st_name_info;
                ReadModule64(symEntryVA, &st_name_info); 
                U32 st_name = (U32)(st_name_info & 0xFFFFFFFF);
                
                char symName[128];
                if (ReadModuleString(strtabAddr + st_name, symName, sizeof(symName))) {
                    symValue = ModuleManager::FindKernelSymbol(symName);
                    
                    if (symValue == 0) {
                        Printk::Write(Printk::Level::LOG_ERR, "KMOD: UNDEFINED SYMBOL: %s\n", symName);
                        return false; 
                    } else {
                        Printk::Write(Printk::Level::LOG_INFO, "KMOD: Resolved '%s' to 0x%016llx\n", symName, symValue);
                    }
                }
            }

            U64 newValue = 0;

            if (type == R_X86_64_RELATIVE) {
                newValue = targetBase + (U64)addend;
            } 
            else if (type == R_X86_64_64 || type == R_X86_64_GLOB_DAT || type == R_X86_64_JUMP_SLOT) {
                newValue = symValue + (U64)addend; 
            } 
            else {
                Printk::Write(Printk::Level::LOG_WARNING, "KMOD: Unhandled RELA type %u\n", type);
                continue;
            }

            if (!WriteModule64(targetVA, newValue)) return false;
        }
        return true;
    };

    // 3. Eksekusi Relokasinya! (Data dulu, baru Fungsi)
    if (!ApplyRelocations(relaAddr, relaSize, "RELA (Data)")) return false;
    if (!ApplyRelocations(jmpRelAddr, jmpRelSize, "JMPREL (PLT)")) return false;

    return true;
}
}

namespace ELF {
    U64 LoadKernelModule(VOID *ELFImage,
                         U64 *KernelPML4,
                         U64 *ImageBaseOut,
                         U64 *ImageEndOut) {
        if (!ELFImage || !KernelPML4) {
            Printk::Write(Printk::Level::LOG_ERR, "KMOD: Invalid parameters\n");
            return ELF_ERR_UNSUPPORTED;
        }

        G_TargetPML4 = KernelPML4;
        ResetModuleAccessCaches();

        auto *EHDR = (ELF64_EHDR *)ELFImage;
        if(EHDR->E_Ident[0] != 0x7F || EHDR->E_Ident[1] != 'E' ||
           EHDR->E_Ident[2] != 'L' || EHDR->E_Ident[3] != 'F') {
            Printk::Write(Printk::Level::LOG_WARNING, "KMOD: Bad magic number!\n");
            return ELF_ERR_BADMAGIC;
        }

        auto *PHDR = reinterpret_cast<ELF64_PHDR *>((UPTR)ELFImage + EHDR->E_Phoff);

        U64 ElfBaseAddr = (U64)-1;
        U64 MaxSegmentEnd = 0;
        bool FoundLoad = false;
        
        for(U16 i = 0; i < EHDR->E_Pthnum; i++){
            if(PHDR[i].P_Type != PT_LOAD) continue;
            FoundLoad = true;
            if(PHDR[i].P_Vaddr < ElfBaseAddr) ElfBaseAddr = PHDR[i].P_Vaddr;
            U64 segEnd = PHDR[i].P_Vaddr + PHDR[i].P_Memsz;
            if (segEnd > MaxSegmentEnd) MaxSegmentEnd = segEnd;
        }

        if(!FoundLoad){
            Printk::Write(Printk::Level::LOG_ERR, "KMOD: No loadable segments found!\n");
            return ELF_ERR_UNSUPPORTED;
        }

        U64 ImageSpanAligned = AlignUp((MaxSegmentEnd > ElfBaseAddr) ? (MaxSegmentEnd - ElfBaseAddr) : PAGE_SIZE, PAGE_SIZE);

        // Hanya mendukung ET_DYN untuk Kernel Module kita (menggunakan PIC/Shared Object)
        if (EHDR->E_Type != ET_DYN) {
            Printk::Write(Printk::Level::LOG_ERR, "KMOD: Hanya mendukung ET_DYN (Shared Object)!\n");
            return ELF_ERR_UNSUPPORTED;
        }

        U64 TargetBaseAddrMode = ReserveDynamicImageBase(ImageSpanAligned);
        U64 ImageRangeEnd = TargetBaseAddrMode + ImageSpanAligned;
        const U8 *ElfBytes = (const U8 *)ELFImage;

        for(U32 i = 0; i < EHDR->E_Pthnum; i++){
            if(PHDR[i].P_Type != PT_LOAD) continue;

            U64 RelocatedVaddr = TargetBaseAddrMode + (PHDR[i].P_Vaddr - ElfBaseAddr);
            U64 VaddrPage = RelocatedVaddr & ~(PAGE_SIZE - 1);

            // SECURITY: Kernel module HARUS berada di Higher Half Memory
            if (VaddrPage < 0xFFFF800000000000ULL) { 
                Printk::Write(Printk::Level::LOG_ERR, 
                    "KMOD SECURITY: Tried to map kernel module outside of higher half at 0x%llx!\n", VaddrPage);
                return ELF_ERR_UNSUPPORTED;
            }

            U64 SizePage = ((RelocatedVaddr + PHDR[i].P_Memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)) - VaddrPage;
            if (!SizePage) continue;

            for(U64 Off = 0; Off < SizePage; Off += PAGE_SIZE){
                U64 CurrentVaddrKernel = VaddrPage + Off;
                UPTR PhysPage = PageAlloc::PhysicalAllocPages(1);
                if(!PhysPage){
                    Printk::Write(Printk::Level::LOG_ERR, "KMOD: Out of memory\n");
                    return ELF_ERR_NOMEM;
                }

                auto *TempKernelPtr = reinterpret_cast<U8*>(HHDM_PhysToVirt(PhysPage));
                String::Memset(TempKernelPtr, 0, PAGE_SIZE);

                U64 CopyStart = (CurrentVaddrKernel > RelocatedVaddr) ? CurrentVaddrKernel : RelocatedVaddr;
                U64 SegmentEnd = RelocatedVaddr + PHDR[i].P_Filesz;
                U64 CopyEnd = ((CurrentVaddrKernel + PAGE_SIZE) < SegmentEnd) ? (CurrentVaddrKernel + PAGE_SIZE) : SegmentEnd;

                if(CopyStart < CopyEnd){
                    U64 CopySize = CopyEnd - CopyStart;
                    U64 SrcOffset = PHDR[i].P_Offset + (CopyStart - RelocatedVaddr);
                    U64 DstOffset = CopyStart - CurrentVaddrKernel;
                    String::Memcpy(TempKernelPtr + DstOffset, ElfBytes + SrcOffset, CopySize);
                }

                // SECURITY: Ganti mapping menjadi Ring 0 saja (Tanpa PAGE_USER)
                U64 PageFlags = PAGE_PRESENT | PAGE_RW; // Read/Write untuk Ring 0
                if(!(PHDR[i].P_Flags & PF_X)) PageFlags |= PAGE_NX; // Non-Executable jika diminta

                if(!PageAlloc::MapPages(KernelPML4, PhysPage, CurrentVaddrKernel, 1, PageFlags)){
                    Printk::Write(Printk::Level::LOG_ERR, "KMOD: Failed to map page\n");
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

        U64 relocatedEntry = TargetBaseAddrMode + (EHDR->E_Entry - ElfBaseAddr);

        if (ImageBaseOut) *ImageBaseOut = TargetBaseAddrMode;
        if (ImageEndOut) *ImageEndOut = ImageRangeEnd;
        
        Printk::Write(Printk::Level::LOG_INFO, "KMOD: Loaded base=0x%016llx entry=0x%016llx\n", 
                      TargetBaseAddrMode, relocatedEntry);
                      
        return relocatedEntry;
    }
}