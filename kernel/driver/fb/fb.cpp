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

/* module name provided via PRINTK_MODULE_NAME */

namespace FB {
    // Cached framebuffer state to avoid repeated BootInfo/HHDM lookups
    static struct {
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

        Printk::Write(Printk::Level::LOG_INFO, "[GOPFB] Framebuffer initialized: %ux%u %u bpp at phys %p (HHDM virt %p)\n",
            (unsigned)FBWidth, (unsigned)FBHeight, (unsigned)FBBitsPerPixel,
            (void*)(UPTR)phys, fb_virt);
    }

    void PutPixel(U32 x, U32 y, U32 rgb){
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
    void Flush(U32 x, U32 y, U32 w, U32 h) {
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

        // If using backbuffer, caller should Flush the destination region as needed.
    }
}
