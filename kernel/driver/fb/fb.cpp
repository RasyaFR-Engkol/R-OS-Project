#define PRINTK_MODULE_NAME "FB"
#include <rosval.h>
#include <rossys.hpp>
#include <framebuffer.hpp>
#include <bootinfo.h>
#include <serial.hpp>
#include <logging.hpp>
#include "../../mm/mm.hpp"
#include "../../mm/kmalloc/kmalloc.hpp"
#include <string.hpp>
#include <spinlock/simple.hpp>

/* module name provided via PRINTK_MODULE_NAME */

namespace FB {
    static Arch::Spinlock::Spinlock g_fb_lock;
    static FlushCallback g_hw_flush_cb = nullptr;
    // Cached framebuffer state to avoid repeated BootInfo/HHDM lookups
    static struct {
        U64 phys_base;
        U8 *fb_base;        // HHDM-mapped framebuffer base (frontbuffer)
        U8 *backbuffer;     // software backbuffer (linear, tightly packed)
        U32 back_pitch;     // bytes per scanline in backbuffer (width * bpp/8)
        U32 width;
        U32 height;
        U32 pitch;          // frontbuffer pitch
        U8  bpp;
        U32 bytes_per_pixel;
        BOOL initialized;
    } state = {};

    // Public Info view exposed via FB::Get()
    static FB::Info g_fb_info = {};

    VOID Init(){
        const BootInfo *bi = BootInfoGet();
        if(!bi || !bi->has_framebuffer) {
            Serial::Printf("No framebuffer info in BootInfo\n");
            return;
        }

        const U64 phys = bi->framebuffer.address;
        const U32 FBWidth = bi->framebuffer.width;
        const U32 FBHeight = bi->framebuffer.height;
        const U32 FBPitch = bi->framebuffer.pitch;
        const U8  FBBitsPerPixel = bi->framebuffer.bpp;

        // Prefer mapping the framebuffer into a new virtual region rather than
        // relying on the global HHDM direct mapping. This lets the driver own
        // the mapping (and attributes) and avoids depending on kmalloc for
        // virtual mappings.
        U8 *fb_virt = nullptr;
        {
            // Compute page-aligned physical base and offset
            UPTR phys_start = (UPTR)phys;
            UPTR phys_page_base = phys_start & PAGE_ADDR_MASK;
            UPTR offset = phys_start - phys_page_base;
            // framebuffer size in bytes (use pitch*height to account for scanline padding)
            SIZE_T fb_bytes = (SIZE_T)FBPitch * (SIZE_T)FBHeight;
            SIZE_T total = offset + fb_bytes;
            SIZE_T pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;

            // Attempt to allocate a contiguous virtual range and map the framebuffer
            void *virt = PageAlloc::VirtualAllocPages(pages);
            if (virt) {
                // Map physical pages into the allocated virtual range
                if (PageAlloc::MapPages(KernelPML4, phys_page_base, (UPTR)virt, pages, PAGE_PRESENT | PAGE_RW)) {
                    fb_virt = (U8*)virt + offset;
                } else {
                    // mapping failed; free the virtual pages and fall back to HHDM
                    PageAlloc::VirtualFreePages(virt, pages);
                    fb_virt = (U8*)HHDM_PhysToVirt((UPTR)phys);
                }
            } else {
                // Could not allocate virtual pages; fall back to HHDM mapping
                fb_virt = (U8*)HHDM_PhysToVirt((UPTR)phys);
            }
        }

        // Cache state for fast access in PutPixel/Rect
        state.phys_base = bi->framebuffer.address;
        state.fb_base = fb_virt;
        state.width = FBWidth;
        state.height = FBHeight;
        state.pitch = FBPitch;
        state.bpp = FBBitsPerPixel;
        state.bytes_per_pixel = (FBBitsPerPixel + 7) / 8;
        // Use the same pitch for the backbuffer as the frontbuffer to avoid
        // any stride/padding mismatch. Some firmwares (and GPUs) pad each
        // scanline to a 4-byte or larger boundary; allocating a tightly
        // packed backbuffer and copying only pixel bytes can produce visual
        // corruption or apparent scaling. Matching the frontbuffer pitch
        // keeps row layout identical and simplifies flush/copy.
        state.back_pitch = state.pitch;
        // Allocate a page-backed backbuffer (contiguous virtual region) and map
        // physical pages for it. We avoid using Kmalloc here so the framebuffer
        // driver controls the mapping explicitly.
        {
            SIZE_T back_bytes = (SIZE_T)state.back_pitch * (SIZE_T)state.height;
            SIZE_T back_pages = (back_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
            state.backbuffer = nullptr;
            if (back_pages > 0) {
                void *vbuf = PageAlloc::VirtualAllocPages(back_pages);
                if (vbuf) {
                    // Allocate contiguous physical pages and map them
                    UPTR phys_back = PageAlloc::PhysicalAllocPages(back_pages);
                    if (phys_back != 0) {
                        if (PageAlloc::MapPages(KernelPML4, phys_back, (UPTR)vbuf, back_pages, PAGE_PRESENT | PAGE_RW)) {
                            state.backbuffer = (U8*)vbuf;
                        } else {
                            // mapping failed; free physical + virtual
                            PageAlloc::PhysicalFreePages(phys_back, back_pages);
                            PageAlloc::VirtualFreePages(vbuf, back_pages);
                        }
                    } else {
                        // couldn't allocate physical pages
                        PageAlloc::VirtualFreePages(vbuf, back_pages);
                    }
                }
            }
            if (!state.backbuffer) {
                Printk::Write(Printk::Level::LOG_WARNING, "[GOPFB] backbuffer allocation failed, will draw directly to frontbuffer\n");
                state.backbuffer = nullptr; // fall back to direct drawing
            }
        }
        state.initialized = TRUE;

    // Populate public info structure
    g_fb_info.base = (volatile U8*)state.fb_base;
    g_fb_info.width = state.width;
    g_fb_info.height = state.height;
    g_fb_info.pitch = state.pitch;
    g_fb_info.bpp = state.bpp;
    g_fb_info.type = 1; // RGB
    g_fb_info.rpos = 16; g_fb_info.rsize = 8;
    g_fb_info.gpos = 8;  g_fb_info.gsize = 8;
    g_fb_info.bpos = 0;  g_fb_info.bsize = 8;

        Printk::Write(Printk::Level::LOG_INFO, "[GOPFB] Framebuffer initialized: %ux%u %u bpp at phys %p (HHDM virt %p)\n",
            (unsigned)FBWidth, (unsigned)FBHeight, (unsigned)FBBitsPerPixel,
            (void*)(UPTR)phys, fb_virt);

        SIZE_T calculated_stride = FBWidth * ((FBBitsPerPixel + 7) / 8);
        Printk::Write(Printk::Level::LOG_INFO, "Pitch: %u, Calculated Stride: %u\n", FBPitch, calculated_stride);

        if (FBPitch == FBWidth * 4 && FBBitsPerPixel == 24) {
            Printk::Write(Printk::Level::LOG_WARNING, "Aneh: Lapor 24BPP tapi Pitch setara 32BPP. Cek pixel format!\n");
        }
    }

    VOID Reconfigure(ConsoleConfig *Config){
        if(!Config) return;

        // --- TAHAP 1: PERSIAPAN (Di luar Lock) ---
        // Kita siapkan mapping dan backbuffer baru dulu biar gak nge-block sistem lama-lama.
        
        // A. Map VRAM Baru (Driver GPU ngasih Alamat Fisik, CPU butuh Virtual)
        U8* new_fb_virt = nullptr;
        SIZE_T fb_bytes = (SIZE_T)Config->Pitch * (SIZE_T)Config->Height;
        SIZE_T pages = (fb_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

        // Coba alokasi virtual address range baru
        void* virt = PageAlloc::VirtualAllocPages(pages);
        if (virt) {
            // Map Physical VRAM ke Virtual baru.
            // PENTING: Gunakan PAGE_PCD (Cache Disable) atau Write-Combining kalau ada,
            // karena ini VRAM GPU. Kalau di-cache CPU, gambar gak bakal muncul real-time.
            if (PageAlloc::MapPages(KernelPML4, Config->FrameBufferAddr, (UPTR)virt, pages, PAGE_PRESENT | PAGE_RW)) {
                 new_fb_virt = (U8*)virt;
            } else {
                 PageAlloc::VirtualFreePages(virt, pages);
            }
        }

        // Fallback: Kalau gagal alloc virtual, pake HHDM (Direct Map)
        if (!new_fb_virt) {
             new_fb_virt = (U8*)HHDM_PhysToVirt(Config->FrameBufferAddr);
             Printk::Write(Printk::Level::LOG_WARNING, "[FB] Reconfigure: Fallback to HHDM mapping\n");
        }

        // B. Buat Backbuffer Baru (Sesuai resolusi baru)
        U8* new_backbuffer = nullptr;
        
        // Alloc Backbuffer Virtual
        void* vback = PageAlloc::VirtualAllocPages(pages);
        if (vback) {
            // Alloc Backbuffer Physical (RAM biasa, jadi boleh di-cache -> Hapus PAGE_PCD)
            UPTR pback = PageAlloc::PhysicalAllocPages(pages);
            if (pback) {
                if (PageAlloc::MapPages(KernelPML4, pback, (UPTR)vback, pages, PAGE_PRESENT | PAGE_RW)) {
                     new_backbuffer = (U8*)vback;
                     // Bersihkan backbuffer baru jadi hitam biar gak sampah
                     String::Memset(new_backbuffer, 0, fb_bytes);
                } else {
                     PageAlloc::PhysicalFreePages(pback, pages);
                     PageAlloc::VirtualFreePages(vback, pages);
                }
            } else {
                 PageAlloc::VirtualFreePages(vback, pages);
            }
        }

        if(!new_backbuffer) {
             Printk::Write(Printk::Level::LOG_WARNING, "[FB] Failed to allocate new backbuffer, direct drawing enabled.\n");
        }

        // --- TAHAP 2: EKSEKUSI (CRITICAL SECTION) ---
        // Sekarang kita matikan interupsi (lewat spinlock) dan tukar pointer.
        {
            Arch::Spinlock::SpinlockGuard Guard(g_fb_lock);

            // [TODO]: Disini idealnya kita Free old_backbuffer & old_fb_virt kalau bukan HHDM.
            // Tapi karena kernel allocator kamu mungkin belum punya `IsHHDM()` check, 
            // kita leak dulu yang lama (GOP framebuffer biasanya kecil, gak fatal).
            // U8* old_backbuffer = state.backbuffer;
            
            // 1. Update State Internal (yang dipake PutPixel)
            state.fb_base = new_fb_virt;
            state.backbuffer = new_backbuffer;
            state.width = Config->Width;
            state.height = Config->Height;
            state.pitch = Config->Pitch;
            state.bpp = Config->Bpp;
            state.bytes_per_pixel = (Config->Bpp + 7) / 8;
            state.back_pitch = state.pitch; // Samakan pitch backbuffer
            state.phys_base = Config->FrameBufferAddr;

            // 2. Update Info Public (yang dipake Compositor/User)
            g_fb_info.base = (volatile U8*)state.fb_base;
            g_fb_info.width = state.width;
            g_fb_info.height = state.height;
            g_fb_info.pitch = state.pitch;
            g_fb_info.bpp = state.bpp;
            
            // Info warna (RGB) biasanya standar, tapi kalau GPU aneh bisa diupdate disini
            state.initialized = TRUE;

            g_hw_flush_cb = Config->OnFlush;
        }

        // Karena masih state FBCon, kita harus bersihin layar dulu sebelum tampilin
        // write string
        FB::Rect(0, 0, state.width, state.height, 0xFF000000);
        FB::Flush(0, 0, state.width, state.height);

        FBConsole::FBConCursorPosition CurPos = FBConsole::GetCursorPosition();
        CurPos.CursorCol = 0;
        CurPos.CursorRow = 0;
        FBConsole::SetCursorPosition(CurPos);

        Printk::Write(Printk::Level::LOG_INFO, "[FB] Reconfigured to %ux%u (Phys: %p, Virt: %p)\n",
            Config->Width, Config->Height, (void*)Config->FrameBufferAddr, new_fb_virt);
    }

    U64 GetPhysAddr() {
        // Kalau mau super strict, pake lock, tapi untuk baca U64 usually atomic enough
        return state.phys_base;
    }

    // Perbaiki FBGetState (kembalikan Value copy, bukan pointer null)
    FB::Info GetStateCopy() {
        Arch::Spinlock::SpinlockGuard Guard(g_fb_lock);
        return g_fb_info; // Return by value (copy struct)
    }
    
    // Atau kalau cuma butuh pointer ke static info yang udah ada:
    const Info* Get() {
        return &g_fb_info;
    }

    static void PutPixel_NoLock(U32 x, U32 y, U32 rgb){
        if (!state.initialized) return;
        if (x >= state.width || y >= state.height) return;

        // prefer backbuffer if available, otherwise draw directly to frontbuffer
        U8 *pixel;
        if (state.backbuffer) {
            pixel = state.backbuffer + (U64)y * state.back_pitch + (U64)x * state.bytes_per_pixel;
        } else {
            pixel = state.fb_base + (U64)y * state.pitch + (U64)x * state.bytes_per_pixel;
        }

        if (state.bpp == 32) {
            /* Use memcpy to avoid unaligned stores via pointer casts which
               trigger -Wcast-align and may be undefined on some architectures. */
            U32 v = rgb;
            String::Memcpy(pixel, &v, sizeof(v));
        } else if (state.bpp == 24) {
            // write B G R (little-endian machine)
            pixel[0] = (U8)(rgb & 0xFF);
            pixel[1] = (U8)((rgb >> 8) & 0xFF);
            pixel[2] = (U8)((rgb >> 16) & 0xFF);
        } else if (state.bpp == 16) {
            // assume RGB565 in low 16 bits of rgb
            U16 v16 = (U16)(rgb & 0xFFFF);
            String::Memcpy(pixel, &v16, sizeof(v16));
        }
    }

    void PutPixel(U32 x, U32 y, U32 rgb){
        Arch::Spinlock::SpinlockGuard GuardMePLSS(g_fb_lock);
        PutPixel_NoLock(x, y, rgb);
    }

    // Faster rectangle fill using per-scanline stores.
    void Rect(U32 x, U32 y, U32 w, U32 h, U32 rgb){
        if (!state.initialized) return;
        if (x >= state.width || y >= state.height) return;
        if (x + w > state.width) w = state.width - x;
        if (y + h > state.height) h = state.height - y;

        // Write into backbuffer/frontbuffer via PutPixel to centralize color packing.
        for (U32 yy = 0; yy < h; ++yy) {
            for (U32 xx = 0; xx < w; ++xx) {
                PutPixel(x + xx, y + yy, rgb);
            }
        }

        // After drawing into backbuffer, flush the region to frontbuffer
        Flush(x, y, w, h);
    }

    // No-flush rectangle fill; writes only to the current drawing surface (backbuffer if present)
    void RectNoFlush(U32 x, U32 y, U32 w, U32 h, U32 rgb){
        if (!state.initialized) return;
        if (x >= state.width || y >= state.height) return;
        if (x + w > state.width) w = state.width - x;
        if (y + h > state.height) h = state.height - y;

        for (U32 yy = 0; yy < h; ++yy) {
            for (U32 xx = 0; xx < w; ++xx) {
                // PutPixel already prefers backbuffer when present, and writes
                // directly to frontbuffer if not. We simply skip flushing here.
                U32 px = x + xx;
                U32 py = y + yy;
                PutPixel(px, py, rgb);
            }
        }
    }

    // Copy a rectangle from backbuffer to framebuffer (frontbuffer).
    static void Flush_NoLock(U32 x, U32 y, U32 w, U32 h) {
        if (!state.initialized) return;
        if (!state.backbuffer) return; // nothing to flush
        if (x >= state.width || y >= state.height) return;
        if (x + w > state.width) w = state.width - x;
        if (y + h > state.height) h = state.height - y;

        U32 bytes = w * state.bytes_per_pixel;
        for (U32 row = 0; row < h; ++row) {
            U8* src = state.backbuffer + (U64)(y + row) * state.back_pitch + (U64)x * state.bytes_per_pixel;
            U8* dst = state.fb_base + (U64)(y + row) * state.pitch + (U64)x * state.bytes_per_pixel;
            String::Memcpy(dst, src, bytes);
        }

        if (g_hw_flush_cb) {
            g_hw_flush_cb(x, y, w, h);
        }
    }

    void Flush(U32 x, U32 y, U32 w, U32 h) {
        Arch::Spinlock::SpinlockGuard FlushGuard(g_fb_lock);
        Flush_NoLock(x, y, w, h);
    }

    void FlushHW(U32 x, U32 y, U32 w, U32 h) {
        if (!state.initialized) return;
        
        // Langsung panggil callback hardware (Virtio Transfer)
        if (g_hw_flush_cb) {
            g_hw_flush_cb(x, y, w, h);
        }
    }

    // Copy region within surface (backbuffer if present, else frontbuffer), overlap-safe
    void CopyRect(U32 src_x, U32 src_y, U32 w, U32 h, U32 dst_x, U32 dst_y) {
        if (!state.initialized) return;
        if (src_x >= state.width || src_y >= state.height) return;
        if (dst_x >= state.width || dst_y >= state.height) return;
        if (src_x + w > state.width) w = state.width - src_x;
        if (src_y + h > state.height) h = state.height - src_y;
        if (dst_x + w > state.width) w = state.width - dst_x;
        if (dst_y + h > state.height) h = state.height - dst_y;
        if (w == 0 || h == 0) return;

        U8* surface;
        U32 pitch;
        if (state.backbuffer) {
            surface = state.backbuffer;
            pitch = state.back_pitch;
        } else {
            surface = state.fb_base;
            pitch = state.pitch;
        }

        const U32 bpp = state.bytes_per_pixel;
        const U32 row_bytes = w * bpp;

        // Determine copy order to be overlap-safe
        if (dst_y > src_y || (dst_y == src_y && dst_x > src_x)) {
            // Copy bottom-up
            for (U32 row = h; row-- > 0; ) {
                U8* src = surface + (U64)(src_y + row) * pitch + (U64)src_x * bpp;
                U8* dst = surface + (U64)(dst_y + row) * pitch + (U64)dst_x * bpp;
                String::Memmove(dst, src, row_bytes);
            }
        } else {
            // Copy top-down
            for (U32 row = 0; row < h; ++row) {
                U8* src = surface + (U64)(src_y + row) * pitch + (U64)src_x * bpp;
                U8* dst = surface + (U64)(dst_y + row) * pitch + (U64)dst_x * bpp;
                String::Memmove(dst, src, row_bytes);
            }
        }

        Flush(dst_x, dst_y, w, h);
    }

    VOID Clear(U32 AARRGGBB){
        Rect(0, 0, state.width, state.height, AARRGGBB);
    }
}
