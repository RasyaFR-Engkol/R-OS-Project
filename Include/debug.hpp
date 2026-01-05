#pragma once
#include <rosval.h>
#include <logging.hpp>

namespace Debug {
	// Hex dump utility: prints memory in hex with optional ASCII at the end of each line.
	// baseAddr: if non-zero, used as the starting address printed at the left.
	void HexDump(const void* ptr, SIZE_T len, SIZE_T bytesPerLine = 16,
				 UPTR baseAddr = 0, bool showAscii = true);

	// Translate a virtual address to physical by walking page tables.
	// Returns TRUE on success and sets physOut, flagsOut and levelOut (4=4KiB, 2=2MiB, 1=1GiB).
	BOOL VirtToPhys(UPTR vaddr, UPTR* physOut, U64* flagsOut = nullptr, SIZE_T* levelOut = nullptr);

	// Dump mappings for a virtual address range (pages number of 4KiB pages).
	// Coalesces contiguous ranges with the same flags and physically contiguous pages.
	void DumpRange(UPTR vstart, SIZE_T pages);

	// Walk top-level tables and print a compact summary (counts per level, present entries).
	void DumpPageTablesSummary();

	// PTE audit tool: scan a region and flag non-present, user bits, NX, large pages etc.
	void AuditPTE(UPTR vstart, SIZE_T pages);

	VOID TestReadPartition(const char* path);
}

namespace ExpectedCrash{
	struct ExpectCrash_T{
		char NameFunction[128];
		U64 Counter;
		U64 ReportMustBeCalled;
	};

	extern ExpectCrash_T Instance[128];
	extern VAL32 InstanceCount;

	ExpectCrash_T *Create(U64 ReportMustBeCalled, const char* functionName);

	VOID Report(ExpectCrash_T *Exp);

	VOID FunctionDone(ExpectCrash_T *Exp);
}

ABI_C VAL32 main_debug_ext2_stress(int argc, char** argv);
