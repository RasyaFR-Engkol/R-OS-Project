#define PRINTK_MODULE_NAME "ACPI-BGRT"

#include "bgrt.hpp"
#include <logging.hpp>
#include <framebuffer.hpp>
#include <mm.hpp>

extern "C" {
    #include "../../acpica/source/include/acpi.h"
}

using namespace Printk;

namespace ACPI {
namespace BGRT {

    // Minimal BMP structures (packed)
    #pragma pack(push,1)
    struct BMPFileHeader {
        U16 bfType;       // 'BM'
        U32 bfSize;
        U16 bfReserved1;
        U16 bfReserved2;
        U32 bfOffBits;    // offset to pixel array
    };

    struct BMPInfoHeader {
        U32  biSize;       // 40 for BITMAPINFOHEADER
        VAL32 biWidth;
        VAL32 biHeight;    // positive: bottom-up, negative: top-down
        U16  biPlanes;     // must be 1
        U16  biBitCount;   // 24 or 32 supported
        U32  biCompression;// 0 = BI_RGB, 3 = BI_BITFIELDS (unsupported here)
        U32  biSizeImage;  // can be 0 for BI_RGB
        VAL32 biXPelsPerMeter;
        VAL32 biYPelsPerMeter;
        U32  biClrUsed;
        U32  biClrImportant;
    };
    #pragma pack(pop)

    static BOOL g_shown = FALSE;

    static inline U32 MakeRGB(U8 r, U8 g, U8 b) {
        return ((U32)r << 16) | ((U32)g << 8) | (U32)b;
    }

    static BOOL DrawBmpToFB(const U8* bmp, U32 fb_off_x, U32 fb_off_y){
        if (!bmp) return FALSE;

        const BMPFileHeader* fh = (const BMPFileHeader*)bmp;
        if (fh->bfType != 0x4D42) { // 'BM'
            Write(Level::LOG_WARNING, " BGRT: BMP magic mismatch (0x%04x)\n", (unsigned)fh->bfType);
            return FALSE;
        }
        const BMPInfoHeader* ih = (const BMPInfoHeader*)(bmp + sizeof(BMPFileHeader));
        if (ih->biSize < 40 || ih->biPlanes != 1) {
            Write(Level::LOG_WARNING, " BGRT: Unsupported BMP header (size=%u planes=%u)\n",
                (unsigned)ih->biSize, (unsigned)ih->biPlanes);
            return FALSE;
        }
        if (!(ih->biBitCount == 24 || ih->biBitCount == 32)) {
            Write(Level::LOG_WARNING, " BGRT: Unsupported bpp=%u (only 24/32)\n", (unsigned)ih->biBitCount);
            return FALSE;
        }
        if (!(ih->biCompression == 0 /*BI_RGB*/)) {
            Write(Level::LOG_WARNING, " BGRT: Unsupported compression=%u (only BI_RGB)\n", (unsigned)ih->biCompression);
            return FALSE;
        }

    const U8* pix = bmp + fh->bfOffBits;
    const VAL32 w = ih->biWidth;
    const VAL32 h = ih->biHeight;
        const BOOL topDown = (h < 0);
        const U32 absH = (U32)(topDown ? -h : h);
        const U16 bpp = ih->biBitCount;
        const U32 bytesPerPixel = (bpp == 24) ? 3u : 4u;
        const U32 stride = (bpp == 24)
            ? ((U32)w * 3u + 3u) & ~3u  // rows padded to 4 bytes
            : (U32)w * 4u;

        auto fb = FB::Get();
        if (!fb) {
            FB::Init();
            fb = FB::Get();
        }
        if (!fb) {
            Write(Level::LOG_WARNING, " BGRT: framebuffer not initialized\n");
            return FALSE;
        }

        // Clip drawing region to framebuffer bounds
        U32 maxX = (w > 0) ? (U32)w : 0;
        U32 maxY = absH;
        if (fb_off_x >= fb->width || fb_off_y >= fb->height) return FALSE;
        if (fb_off_x + maxX > fb->width) maxX = fb->width - fb_off_x;
        if (fb_off_y + maxY > fb->height) maxY = fb->height - fb_off_y;

        for (U32 row = 0; row < maxY; ++row) {
            U32 srcRow = topDown ? row : (absH - 1u - row);
            const U8* src = pix + (U32)srcRow * stride;
            for (U32 col = 0; col < maxX; ++col) {
                const U8* p = src + col * bytesPerPixel;
                U8 b = p[0], g = p[1], r = p[2];
                U32 rgb = MakeRGB(r, g, b);
                // ignore alpha if 32bpp
                FB::PutPixel(fb_off_x + col, fb_off_y + row, rgb);
            }
        }
        return TRUE;
    }

    BOOL ShowLogoOnce(){
        if (g_shown) return TRUE;

        ACPI_TABLE_BGRT* bgrt = nullptr;
        ACPI_STATUS st = AcpiGetTable((ACPI_STRING)ACPI_SIG_BGRT, 0, (ACPI_TABLE_HEADER**)&bgrt);
        if (ACPI_FAILURE(st) || !bgrt) {
            return FALSE;
        }

        // Only draw if the firmware provided an image address
        if (!bgrt->ImageAddress) {
            return FALSE;
        }

        // Map physical address via HHDM and draw
        const U8* bmp = (const U8*)HHDM_PhysToVirt((UPTR)bgrt->ImageAddress);
        BOOL ok = DrawBmpToFB(bmp, (U32)bgrt->ImageOffsetX, (U32)bgrt->ImageOffsetY);
        if (ok) {
            g_shown = TRUE;
            Write(Level::LOG_INFO, " BGRT: Logo drawn at (%u,%u)\n",
                (unsigned)bgrt->ImageOffsetX, (unsigned)bgrt->ImageOffsetY);
        }
        return ok;
    }

}
}
