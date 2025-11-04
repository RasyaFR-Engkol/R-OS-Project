#pragma once

#include <rosval.h>
#include <bootinfo.h>

namespace FB {
    struct Info {
        volatile U8* base;   // HHDM-mapped base
        U32 pitch;
        U32 width;
        U32 height;
        U8  bpp;     // 24 or 32 typically
        U8  type;    // 1 = RGB
        U8  rpos, rsize;
        U8  gpos, gsize;
        U8  bpos, bsize;
    };

    // Initialize from BootInfo (GOP/Multiboot2 framebuffer).
    // Returns TRUE on success, FALSE if unsupported.
    VOID Init();

    // Clear the screen with a 0x00RRGGBB color.
    void Clear(U32 rgb);

    // Put a pixel at (x,y) with 0x00RRGGBB.
    void PutPixel(U32 x, U32 y, U32 rgb);

    // Fill rectangle.
    void Rect(U32 x, U32 y, U32 w, U32 h, U32 rgb);

    // Fill rectangle into backbuffer only (no immediate flush).
    // If no backbuffer is present, draws directly to frontbuffer without extra copies.
    void RectNoFlush(U32 x, U32 y, U32 w, U32 h, U32 rgb);

    // Flush a rectangular region from backbuffer to frontbuffer (HHDM).
    void Flush(U32 x, U32 y, U32 w, U32 h);

    // Copy a rectangle within the framebuffer/backbuffer surface. Overlap-safe.
    // Source and destination may overlap; copying is handled correctly.
    // When a backbuffer exists, copies inside the backbuffer and the destination
    // area should be flushed by the caller if needed.
    void CopyRect(U32 src_x, U32 src_y, U32 w, U32 h, U32 dst_x, U32 dst_y);

    // Query current mode; returns nullptr if not initialized.
    const Info* Get();
}

namespace FBConsole {
    VOID Init();
    VOID WriteString(const CHAR8 *s);
}
