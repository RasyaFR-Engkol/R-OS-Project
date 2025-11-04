
#include "mm.hpp"
#include <bootinfo.h>
#include <serial.hpp>
#include <rossys.hpp>

// Bitmap-backed physical frame allocator.
// Default conservative config: manage frames starting at 1 MiB for up to 1 GiB
// of RAM (adjust PHYS_BASE/PHYS_FRAMES to fit your platform).

namespace {
	// Physical memory region start (common early kernel start)
	constexpr UPTR PHYS_BASE = (UPTR)0x00100000ULL; // 1 MiB
	// Number of physical frames managed (262144 * 4096 = 1 GiB)
	constexpr SIZE_T PHYS_FRAMES = 262144;
	constexpr SIZE_T BITMAP_BYTES = (PHYS_FRAMES + 7) / 8;

	static U8 phys_bitmap[BITMAP_BYTES];

	inline void phys_bitmap_set(SIZE_T idx){ phys_bitmap[idx >> 3] |= (U8)(1u << (idx & 7)); }
	inline void phys_bitmap_clear(SIZE_T idx){ phys_bitmap[idx >> 3] &= (U8)~(1u << (idx & 7)); }
	inline bool phys_bitmap_test(SIZE_T idx){ return (phys_bitmap[idx >> 3] >> (idx & 7)) & 1u; }
}

namespace PageAlloc{
	void Physical() {
		// Default: mark all frames reserved (1)
		for (SIZE_T i = 0; i < BITMAP_BYTES; ++i) phys_bitmap[i] = 0xFF;

		const BootInfo* bi = BootInfoGet();
		if (bi && bi->has_memmap) {
			// Enable frames only in E820 available regions, within managed range
			for (U32 i = 0; i < bi->memmap.count; ++i) {
				const BootMemRegion* r = &bi->memmap.regions[i];
				if (r->type != 1 || r->length == 0) continue; // only usable RAM

				U64 start = r->base;
				U64 end   = r->base + r->length; // exclusive
				if (end <= PHYS_BASE) continue;
				if (start < PHYS_BASE) start = PHYS_BASE;

				// Clamp to managed window
				U64 managed_end = (U64)PHYS_BASE + (U64)PHYS_FRAMES * (U64)PAGE_SIZE;
				if (start >= managed_end) continue;
				if (end > managed_end) end = managed_end;

				// Page align
				start = (start + PAGE_SIZE - 1) & ~((U64)PAGE_SIZE - 1);
				end   = end & ~((U64)PAGE_SIZE - 1);
				if (end <= start) continue;

				SIZE_T first = (SIZE_T)((start - PHYS_BASE) / PAGE_SIZE);
				SIZE_T last  = (SIZE_T)((end   - PHYS_BASE) / PAGE_SIZE);
				for (SIZE_T f = first; f < last; ++f) phys_bitmap_clear(f);
			}
		} else {
			// Fallback: mark all as free like before, but keep behavior predictable
			for (SIZE_T i = 0; i < BITMAP_BYTES; ++i) phys_bitmap[i] = 0;
		}

		// Always reserve framebuffer as it's MMIO, not RAM
		if (bi && bi->has_framebuffer) {
			U64 fb = bi->framebuffer.address;
			U64 fb_size = (U64)bi->framebuffer.pitch * (U64)bi->framebuffer.height;
			U64 fb_lo = fb & ~((U64)PAGE_SIZE - 1);
			U64 fb_hi = (fb + fb_size + PAGE_SIZE - 1) & ~((U64)PAGE_SIZE - 1);
			U64 managed_end = (U64)PHYS_BASE + (U64)PHYS_FRAMES * (U64)PAGE_SIZE;
			if (fb_lo < managed_end && fb_hi > PHYS_BASE) {
				if (fb_lo < PHYS_BASE) fb_lo = PHYS_BASE;
				if (fb_hi > managed_end) fb_hi = managed_end;
				SIZE_T first = (SIZE_T)((fb_lo - PHYS_BASE) / PAGE_SIZE);
				SIZE_T last  = (SIZE_T)((fb_hi - PHYS_BASE) / PAGE_SIZE);
				for (SIZE_T f = first; f < last; ++f) phys_bitmap_set(f);
			}
		}

		Serial::Write("[phys] initialized from E820 (managed 1GiB@1MiB)\n");
	}

	UPTR PhysicalAllocPages(SIZE_T count) {
		if (count == 0 || count > PHYS_FRAMES) return (UPTR)0;

		LOCKRFLAGS _irq = Arch::SaveAndDisableInterrupts();

		SIZE_T run = 0;
		for (SIZE_T i = 0; i < PHYS_FRAMES; ++i) {
			if (!phys_bitmap_test(i)) {
				++run;
				if (run == count) {
					SIZE_T start = i + 1 - count;
					for (SIZE_T j = start; j <= i; ++j) phys_bitmap_set(j);
					UPTR addr = PHYS_BASE + (UPTR)start * (UPTR)PAGE_SIZE;
					Arch::RestoreInterrupts(_irq);
					return addr;
				}
			} else {
				run = 0;
			}
		}
		Arch::RestoreInterrupts(_irq);
		return (UPTR)0; // not found
	}

	void PhysicalFreePages(UPTR addr, SIZE_T count) {
		if (addr == 0 || count == 0) return;
		if (addr < PHYS_BASE) return;
		SIZE_T idx = (SIZE_T)((addr - PHYS_BASE) / PAGE_SIZE);
		if (idx + count > PHYS_FRAMES) return;
		LOCKRFLAGS _irq = Arch::SaveAndDisableInterrupts();
		for (SIZE_T i = 0; i < count; ++i) phys_bitmap_clear(idx + i);
		Arch::RestoreInterrupts(_irq);
	}

	void PhysicalReserve(UPTR addr, SIZE_T count) {
		if (addr == 0 || count == 0) return;
		if (addr < PHYS_BASE) return; // out of managed range
		SIZE_T idx = (SIZE_T)((addr - PHYS_BASE) / PAGE_SIZE);
		if (idx + count > PHYS_FRAMES) return;
		LOCKRFLAGS _irq = Arch::SaveAndDisableInterrupts();
		for (SIZE_T i = 0; i < count; ++i) phys_bitmap_set(idx + i);
		Arch::RestoreInterrupts(_irq);
	}
}

