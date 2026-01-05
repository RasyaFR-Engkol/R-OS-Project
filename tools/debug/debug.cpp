#include <rosval.h>
#include <logging.hpp>
#include <string.hpp>
#include <mm.hpp>
#include <serial.hpp>
#include <debug.hpp>
#include <filesystem/filesystem.hpp>

namespace Debug {
	static inline void printLineAddr(UPTR addr) {
		Serial::Printf("%p: ", (void*)(uintptr_t)addr);
	}

	void HexDump(const void* ptr, SIZE_T len, SIZE_T bytesPerLine, UPTR baseAddr, bool showAscii) {
		const U8* p = (const U8*)ptr;
		if (bytesPerLine == 0) bytesPerLine = 16;
		for (SIZE_T i = 0; i < len; i += bytesPerLine) {
			SIZE_T line = (i + bytesPerLine <= len) ? bytesPerLine : (len - i);
			UPTR addr = baseAddr ? (baseAddr + i) : (UPTR)(uintptr_t)(p + i);
			printLineAddr(addr);
			// hex area
			for (SIZE_T j = 0; j < bytesPerLine; ++j) {
				if (j < line) Serial::Printf("%02x ", (unsigned)p[i + j]);
				else Serial::Write("   ");
			}
			// spacer
			Serial::Write(" ");
			// ascii area
			if (showAscii) {
				for (SIZE_T j = 0; j < line; ++j) {
					U8 c = p[i + j];
					if (c < 32 || c > 126) c = '.';
					Serial::Printf("%c", (char)c);
				}
			}
			Serial::Write("\n");
		}
	}

	// Helpers to walk page tables
	static inline U64* tableFromPhys(UPTR phys) {
		return (U64*)HHDM_PhysToVirt(phys & PAGE_ADDR_MASK);
	}

	BOOL VirtToPhys(UPTR vaddr, UPTR* physOut, U64* flagsOut, SIZE_T* levelOut) {
		if (!KernelPML4) return FALSE;
		U64* pml4 = KernelPML4;
		SIZE_T i4 = (vaddr >> 39) & 0x1ff;
		SIZE_T i3 = (vaddr >> 30) & 0x1ff;
		SIZE_T i2 = (vaddr >> 21) & 0x1ff;
		SIZE_T i1 = (vaddr >> 12) & 0x1ff;
		U64 off4k = vaddr & 0xfffULL;
		U64 e4 = pml4[i4];
		if (!(e4 & PAGE_PRESENT)) return FALSE;
		U64* pdpt = tableFromPhys((UPTR)e4);
		U64 e3 = pdpt[i3];
		if (!(e3 & PAGE_PRESENT)) return FALSE;
		if (e3 & PAGE_PS) {
			// 1GiB page
			UPTR phys = (UPTR)(e3 & 0x000fffffc0000000ULL) | (vaddr & 0x3fffffffULL);
			if (physOut) *physOut = phys;
			if (flagsOut) *flagsOut = e3;
			if (levelOut) *levelOut = 1;
			return TRUE;
		}
		U64* pd = tableFromPhys((UPTR)e3);
		U64 e2 = pd[i2];
		if (!(e2 & PAGE_PRESENT)) return FALSE;
		if (e2 & PAGE_PS) {
			// 2MiB page
			UPTR phys = (UPTR)(e2 & 0x000fffffffe00000ULL) | (vaddr & 0x1fffffULL);
			if (physOut) *physOut = phys;
			if (flagsOut) *flagsOut = e2;
			if (levelOut) *levelOut = 2;
			return TRUE;
		}
		U64* pt = tableFromPhys((UPTR)e2);
		U64 e1 = pt[i1];
		if (!(e1 & PAGE_PRESENT)) return FALSE;
		UPTR phys = (UPTR)(e1 & PAGE_ADDR_MASK) | off4k;
		if (physOut) *physOut = phys;
		if (flagsOut) *flagsOut = e1;
		if (levelOut) *levelOut = 4;
		return TRUE;
	}

	static const char* flagStr(U64 f) {
		static char buf[32];
		char* s = buf; SIZE_T n=0;
		auto add=[&](char c){ if(n<sizeof(buf)-1){*s++=c; n++;} };
		if (f & PAGE_PRESENT) add('P'); else add('-');
		if (f & PAGE_RW) add('W'); else add('R');
		if (f & PAGE_USER) add('U'); else add('S');
		if (f & PAGE_PWT) add('T');
		if (f & PAGE_PCD) add('C');
		if (f & PAGE_ACCESSED) add('A');
		if (f & PAGE_DIRTY) add('D');
		if (f & PAGE_GLOBAL) add('G');
		if (f & PAGE_NX) add('X'); else add('x');
		*s = 0;
		return buf;
	}

	void DumpRange(UPTR vstart, SIZE_T pages) {
		if (pages == 0) return;
		UPTR v = vstart;
		SIZE_T left = pages;
		while (left) {
			UPTR phys; U64 flags; SIZE_T lvl;
			if (!VirtToPhys(v, &phys, &flags, &lvl)) {
				Serial::Printf("%p -> not-present\n", (void*)(uintptr_t)v);
				v += PAGE_SIZE; left--; continue;
			}
			// Try to coalesce contiguous 4K pages
			SIZE_T run = 1;
			while (run < left) {
				UPTR nphys; U64 nflags; SIZE_T nlvl;
				if (!VirtToPhys(v + run*PAGE_SIZE, &nphys, &nflags, &nlvl)) break;
				if (nflags != flags) break;
				if (nphys != phys + run*PAGE_SIZE) break;
				run++;
			}
			Serial::Printf("%p..%p -> phys %p flags %s lvl%c pages=%u\n",
				(void*)(uintptr_t)v,
				(void*)(uintptr_t)(v + run*PAGE_SIZE - 1),
				(void*)(uintptr_t)phys,
				flagStr(flags),
				(lvl==4?'4':(lvl==2?'2':'1')),
				(unsigned)run);
			v += run*PAGE_SIZE;
			left -= run;
		}
	}

	void DumpPageTablesSummary() {
		if (!KernelPML4) { Serial::Write("[PTE] No KernelPML4\n"); return; }
		U64 p4 = 0, p3 = 0, p2 = 0, p1 = 0, huge1g = 0, huge2m = 0;
		for (SIZE_T i4 = 0; i4 < 512; ++i4) {
			U64 e4 = KernelPML4[i4];
			if (!(e4 & PAGE_PRESENT)) continue;
			p4++;
			U64* pdpt = tableFromPhys((UPTR)e4);
			for (SIZE_T i3 = 0; i3 < 512; ++i3) {
				U64 e3 = pdpt[i3];
				if (!(e3 & PAGE_PRESENT)) continue;
				p3++;
				if (e3 & PAGE_PS) { huge1g++; continue; }
				U64* pd = tableFromPhys((UPTR)e3);
				for (SIZE_T i2 = 0; i2 < 512; ++i2) {
					U64 e2 = pd[i2];
					if (!(e2 & PAGE_PRESENT)) continue;
					p2++;
					if (e2 & PAGE_PS) { huge2m++; continue; }
					U64* pt = tableFromPhys((UPTR)e2);
					for (SIZE_T i1 = 0; i1 < 512; ++i1) {
						U64 e1 = pt[i1];
						if (e1 & PAGE_PRESENT) p1++;
					}
				}
			}
		}
		Serial::Printf("[PTE] Summary: PML4=%llu PDPT=%llu PD=%llu PT(Present)=%llu 1G=%llu 2M=%llu\n",
			p4, p3, p2, p1, huge1g, huge2m);
	}

	void AuditPTE(UPTR vstart, SIZE_T pages) {
		Serial::Printf("[PTE] Audit start=%p pages=%u\n", (void*)(uintptr_t)vstart, (unsigned)pages);
		UPTR v = vstart;
		for (SIZE_T i = 0; i < pages; ++i, v += PAGE_SIZE) {
			UPTR phys; U64 flags; SIZE_T lvl;
			if (!VirtToPhys(v, &phys, &flags, &lvl)) {
				Serial::Printf("  %p: not-present\n", (void*)(uintptr_t)v);
				continue;
			}
			bool large = (lvl != 4);
			Serial::Printf("  %p: phys=%p flags=%s %s\n",
				(void*)(uintptr_t)v,
				(void*)(uintptr_t)phys,
				flagStr(flags),
				large ? (lvl==2?"[2M]":"[1G]") : "");
		}
		Serial::Write("[PTE] Audit done\n");
	}

	VOID TestReadPartition(const char* path){
        U32 pcount = PartitionManager::GetPartitionCount();
        for(U32 pi = 0; pi < pcount; ++pi){
            Partition* part = PartitionManager::GetPartitionByIndex(pi);
            if(!part) continue;
            FileSystem* fs = part->GetFilesystem();
            if(!fs) continue;
            Printk::Write(Printk::Level::LOG_INFO, "FAT32 Demo: trying to open /EFI/BOOT/grub.cfg on partition %u\n", pi);
            File* f = fs->Open(path, O_RDWR);
            if(!f){
                Printk::Write(Printk::Level::LOG_INFO, "FAT32 Demo: grub.cfg not found on partition %u\n", pi);
                continue;
            }
            U32 size = (U32)f->FileSize;
            void* buf = Kmalloc::Alloc(size ? size : 1);
            if(!buf){
                Printk::Write(Printk::Level::LOG_ERR, "FAT32 Demo: allocation failed for size %u\n", size);
                fs->Close(f);
                continue;
            }
            U32 got = fs->Read(f, (U8*)buf, size);
            Printk::Write(Printk::Level::LOG_INFO, "FAT32 Demo: read %u/%u bytes from grub.cfg\n", got, size);
            if(got) Debug::HexDump(buf, got, 16, 0, true);
            fs->Close(f);
            Kmalloc::Free(buf);
            break; // only first fs
        }
    }
}

namespace ExpectedCrash{
	ExpectCrash_T Instance[128] = {};
	VAL32 InstanceCount = 0;

	ExpectCrash_T* Create(U64 ReportMustBeCalled, const char* functionName){
		if (!functionName) return nullptr;

		// If an entry for this function name already exists, reuse it
		for (VAL32 i = 0; i < InstanceCount; i++){
			if (String::Strcmp(Instance[i].NameFunction, functionName) == 0){
				Instance[i].ReportMustBeCalled = ReportMustBeCalled;
				Instance[i].Counter = 0; // reset previous counter when reconfiguring
				return &Instance[i];
			}
		}

		if (InstanceCount >= 128) return nullptr;

		ExpectCrash_T* exp = &Instance[InstanceCount++];
		exp->Counter = 0;
		exp->ReportMustBeCalled = ReportMustBeCalled;
		// copy name safely and ensure null-termination
		String::Strncpy(exp->NameFunction, functionName, sizeof(exp->NameFunction));
		exp->NameFunction[sizeof(exp->NameFunction) - 1] = '\0';
		return exp;
	}

	VOID Report(ExpectCrash_T *Exp){
		if(!Exp) return;
		if(Exp->ReportMustBeCalled == 0) return; // nothing to track

		// Prefer validating the pointer belongs to our Instance array.
		ExpectCrash_T* target = nullptr;
		for (VAL32 i = 0; i < InstanceCount; i++){
			if (&Instance[i] == Exp){ target = &Instance[i]; break; }
		}
		// Fallback: try to match by name if pointer not found (backwards compatibility)
		if (!target){
			for (VAL32 i = 0; i < InstanceCount; i++){
				if (String::Strcmp(Instance[i].NameFunction, Exp->NameFunction) == 0){ target = &Instance[i]; break; }
			}
		}
		if (!target) return; // unknown/invalid Exp pointer

		target->Counter++;
		if (target->Counter >= target->ReportMustBeCalled){
			Serial::Printf("EXPECTED CRASH: Function %s expected to crash after %llu calls. (called %llu times)\n",
				target->NameFunction,
				target->ReportMustBeCalled,
				target->Counter
			);
			// reset counter so we don't repeatedly trigger
			target->Counter = 0;
		}
	}

	VOID FunctionDone(ExpectCrash_T *Exp){
		if(!Exp) return;
		// Find the instance by pointer first, fallback to name match
		for(VAL32 i = 0; i < InstanceCount; i++){
			if (&Instance[i] == Exp || String::Strcmp(Instance[i].NameFunction, Exp->NameFunction) == 0){
				Instance[i].Counter = 0;
				return;
			}
		}
	}
}