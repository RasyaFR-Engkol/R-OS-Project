#include "fbcon.hpp"
#define PRINTK_MODULE_NAME "FBConsole"
#include <rosval.h>
#include <rossys.hpp>
#include <bootinfo.h>
#include <framebuffer.hpp>
#include "font.h"
#include <mm.hpp>
#include <logging.hpp>
#include <string.hpp>
#include <rossys.hpp>

/* module name provided via PRINTK_MODULE_NAME */

namespace FBConsole {
    // Untuk sementara taro PSF1 Header disini aja
    #define PSF1_MAGIC0 0x36
    #define PSF1_MAGIC1 0x04
    #define PSF1_MODE_512 0x01

    typedef struct __attribute__((packed)) {
        uint8_t magic[2];   // 0x36, 0x04
        uint8_t mode;       // bit0 => 512 glyphs if set, else 256
        uint8_t charsize;   // bytes per glyph
    } PSF1_HEADER;

    // Runtime PSF1/font state for console rendering
    static U8 *g_psf_font = nullptr;       // pointer to glyph bytes (after header)
    static unsigned g_psf_glyphs = 0;
    static unsigned g_psf_charsize = 0;    // rows per glyph
    static unsigned g_psf_charwidth = 8;   // PSF1 fonts are 8 pixels wide
    static BOOL g_ready = FALSE;
    static U32 g_fg = 0x00FFFFFF; // white
    static U32 g_bg = 0x00000000; // black
    // Screen and text-grid geometry
    static U32 g_scr_w = 0, g_scr_h = 0;
    static U32 g_cell_w = 8;  // default: 8px glyph, no extra spacing
    static U32 g_cell_h = 16; // default: glyph rows, no extra spacing
    static U32 g_cols = 0, g_rows = 0;
    // Cursor in cell coordinates
    static U32 g_cur_col = 0;
    static U32 g_cur_row = 0;
    // Text buffer (grid of chars)
    static CHAR8* g_grid = nullptr;
    static CHAR8 g_cursor_char = '_';
    static BOOL g_cursor_visible = TRUE;
    static U64 g_last_blink_time = 0;
    static U64 g_blink_interval_tick = 0;

    // Forward declarations
    static inline void RenderCell(U32 col, U32 row);
    static inline void RenderAll();
    static inline void NewLine();
    static inline void ScrollOne();
    static inline void RenderCursor(BOOL show);
    static inline void HideCursor();
    static inline void ShowCursor();

    VOID Init(){
        SIZE_T fontlen =Lat15_VGA16_psf_len;
        U8 *fontdata = (U8*)Kmalloc::Alloc(fontlen);
        if(!fontdata){
            Printk::Write(Printk::Level::LOG_EMERG, "Failed to allocate memory\n");
            // Gak akan pernah sampe sini
            return;
        }
        String::Memcpy(fontdata, Lat15_VGA16_psf, fontlen);

        Printk::Write(Printk::Level::LOG_INFO, "Font loaded, size %u bytes\n", (unsigned)fontlen);

        // Validasi font
        /* Basic validation: ensure we at least have a PSF1 header */
        if (fontlen < sizeof(FBConsole::PSF1_HEADER)) {
            Printk::Write(Printk::Level::LOG_EMERG, "Font too small for PSF1 header (len=%u)\n", (unsigned)fontlen);
            return;
        }

        FBConsole::PSF1_HEADER *HandlePSF = (FBConsole::PSF1_HEADER*)fontdata;

        /* Correct magic checks: compare magic[0] and magic[1] */
        if (HandlePSF->magic[0] != PSF1_MAGIC0 || HandlePSF->magic[1] != PSF1_MAGIC1) {
            Printk::Write(Printk::Level::LOG_EMERG, "Invalid PSF1 magic: %02x %02x\n",
                (unsigned)HandlePSF->magic[0], (unsigned)HandlePSF->magic[1]);
            return;
        }

        /* Compute expected total PSF1 size and validate it matches provided font length */
        unsigned glyphs = (HandlePSF->mode & PSF1_MODE_512) ? 512u : 256u;
        size_t expected = sizeof(FBConsole::PSF1_HEADER) + (size_t)glyphs * (size_t)HandlePSF->charsize;
        if (fontlen < expected) {
            Printk::Write(Printk::Level::LOG_EMERG, "PSF1 font truncated: got %u bytes, need %u\n",
                (unsigned)fontlen, (unsigned)expected);
            return;
        }

        /* Store runtime info for rendering later */
        g_psf_glyphs = glyphs;
        g_psf_charsize = (unsigned)HandlePSF->charsize;
        g_psf_font = fontdata + sizeof(FBConsole::PSF1_HEADER);
        // Tighter spacing: no extra pixels horizontally/vertically
        g_cell_w = g_psf_charwidth;   // 8px typical for PSF1
        g_cell_h = g_psf_charsize;    // e.g., 16 rows

        /* Clear the screen to black (if framebuffer available). We use BootInfo to
         * query resolution because FB::Get() isn't exposed here. FB::Rect will
         * still work if FB has been initialized previously. */
        const BootInfo *bi = BootInfoGet();
        if (bi && bi->has_framebuffer) {
            // prefer using FB::Rect which will draw into backbuffer if present
            g_scr_w = bi->framebuffer.width;
            g_scr_h = bi->framebuffer.height;
            FB::Rect(0, 0, g_scr_w, g_scr_h, g_bg);
        }

        // Compute columns/rows and allocate text grid
        if (g_scr_w && g_scr_h) {
            g_cols = g_scr_w / g_cell_w;
            if (g_cols == 0) g_cols = 1;
            g_rows = g_scr_h / g_cell_h;
            if (g_rows == 0) g_rows = 1;
            SIZE_T grid_bytes = (SIZE_T)g_cols * (SIZE_T)g_rows;
            g_grid = (CHAR8*)Kmalloc::Alloc(grid_bytes);
            if (g_grid) {
                String::Memset(g_grid, ' ', grid_bytes);
            }
        }

        // Mark console ready so other code (or Printk) can detect it
        g_ready = TRUE;

        g_blink_interval_tick = (U64)Arch::Time::TickHz() / 2;
        if (g_blink_interval_tick == 0) {
            g_blink_interval_tick = 50; // Fallback (assuming 100Hz)
        }
        g_last_blink_time = Arch::Time::NowTicks();
        ShowCursor();

        // Optionally print a small test string both to serial (Printk) and screen
        Printk::Write(Printk::Level::LOG_INFO, " PSF font ready (%u glyphs, %u rows)\n",
            (unsigned)g_psf_glyphs, (unsigned)g_psf_charsize);

        // We no longer draw a manual " Ready" string here. Instead
        // rely on Printk::Write (called above) which will mirror the same
        // message to the framebuffer via FBConsole::WriteString(). This
        // avoids duplicate on-screen messages and keeps logging centralized.
    }

    BOOL IsReady() {
        return g_ready;
    }

    // Draw a single ASCII character at pixel coordinates (x,y)
    static inline VOID PutCharAt(U32 x, U32 y, CHAR8 c, U32 color) {
        if (!g_ready || !g_psf_font) return;
        unsigned code = (unsigned)(unsigned char)c;
        if (code >= g_psf_glyphs) code = (unsigned)'?';
        // Clear cell background (no flush yet)
        FB::RectNoFlush(x, y, g_cell_w, g_cell_h, g_bg);
        for (unsigned row = 0; row < g_psf_charsize; ++row) {
            U8 glyph = g_psf_font[code * g_psf_charsize + row];
            for (unsigned bit = 0; bit < g_psf_charwidth; ++bit) {
                if (glyph & (1u << (7u - bit))) {
                    FB::PutPixel(x + bit, y + row, color);
                }
            }
        }
        // Note: Do not flush here; caller decides batching vs per-cell flush.
    }

    static inline void RenderCell(U32 col, U32 row) {
        if (!g_grid) return;
        if (col >= g_cols || row >= g_rows) return;
        U32 px = col * g_cell_w;
        U32 py = row * g_cell_h;
        PutCharAt(px, py, g_grid[row * g_cols + col], g_fg);
        FB::Flush(px, py, g_cell_w, g_cell_h);
    }

    static inline void RenderAll() {
        if (!g_grid) return;
        for (U32 r = 0; r < g_rows; ++r) {
            for (U32 c = 0; c < g_cols; ++c) {
                U32 px = c * g_cell_w;
                U32 py = r * g_cell_h;
                PutCharAt(px, py, g_grid[r * g_cols + c], g_fg);
            }
        }
        FB::Flush(0, 0, g_scr_w, g_scr_h);
    }

    static inline void NewLine() {
        g_cur_col = 0;
        if (++g_cur_row >= g_rows) {
            ScrollOne();
        }
    }

    static inline void ScrollOne() {
        if (!g_grid || g_rows == 0) return;
        SIZE_T row_bytes = (SIZE_T)g_cols;
        SIZE_T total_bytes = row_bytes * (SIZE_T)g_rows;
        // Move rows up by one in the text buffer
        String::Memmove(g_grid, g_grid + row_bytes, total_bytes - row_bytes);
        // Clear last row in the text buffer
        String::Memset(g_grid + total_bytes - row_bytes, ' ', row_bytes);

        // Visually scroll using pixel copy: copy all but first text-row upwards by cell height
        if (g_scr_w && g_scr_h) {
            U32 copy_h = g_scr_h - g_cell_h;
            if (copy_h > 0) {
                FB::CopyRect(0, g_cell_h, g_scr_w, copy_h, 0, 0);
                // Clear the last row area (no flush yet)
                FB::RectNoFlush(0, g_scr_h - g_cell_h, g_scr_w, g_cell_h, g_bg);
                // Flush the scrolled region (top area)
                FB::Flush(0, 0, g_scr_w, copy_h);
            }
        }

        // Re-render the last row characters without full redraw
        U32 last_row = g_rows - 1;
        for (U32 c = 0; c < g_cols; ++c) {
            U32 px = c * g_cell_w;
            U32 py = last_row * g_cell_h;
            PutCharAt(px, py, g_grid[last_row * g_cols + c], g_fg);
        }
        // Flush only the last row band once
        FB::Flush(0, last_row * g_cell_h, g_scr_w, g_cell_h);

        g_cur_row = g_rows - 1;
        g_cur_col = 0;
    }

    static inline void RenderCursor(BOOL show){
        if(!g_grid) return;
        if(g_cur_col >= g_cols || g_cur_row >= g_rows) return;

        U32 Px = g_cur_col * g_cell_w;
        U32 Py = g_cur_row * g_cell_h;

        CHAR8 CharToDraw;
        if(show){
            CharToDraw = g_cursor_char;
        } else {
            CharToDraw = g_grid[g_cur_row * g_cols + g_cur_col];
        }

        PutCharAt(Px, Py, CharToDraw, g_fg);
        FB::Flush(Px, Py, g_cell_w, g_cell_h);
    }

    static inline void HideCursor(){
        if(!g_cursor_visible) return;
        RenderCursor(FALSE);
        g_cursor_visible = FALSE;
    }

    static inline void ShowCursor(){
        // If already visible, don't reset the blink timer - frequent
        // ShowCursor() calls (e.g. from logging) shouldn't prevent the
        // regular blink toggle. Only render and update state when the
        // visibility actually changes.
        if (g_cursor_visible) return;
        RenderCursor(TRUE); // draw underscore
        g_cursor_visible = TRUE;
        g_last_blink_time = Arch::Time::NowTicks();
    }

    // Write a zero-terminated string with newline, wrap and scrolling
    VOID WriteString(const CHAR8 *s) {
        if (!g_ready || !s || !g_grid) return;

        HideCursor();

        for (const CHAR8* p = s; *p; ++p) {
            CHAR8 ch = *p;
            if (ch == '\b') {
                // Handle backspace: move cursor left (or to end of previous line), erase cell
                if (g_cur_col > 0) {
                    --g_cur_col;
                } else if (g_cur_row > 0) {
                    --g_cur_row;
                    g_cur_col = (g_cols ? (g_cols - 1) : 0);
                } else {
                    // At top-left; nothing to erase
                    continue;
                }
                g_grid[g_cur_row * g_cols + g_cur_col] = ' ';
                RenderCell(g_cur_col, g_cur_row);
                continue;
            }
            if (ch == '\r') {
                g_cur_col = 0;
                continue;
            }
            if (ch == '\n') {
                NewLine();
                continue;
            }
            if (ch == '\t') {
                // MODIFIKASI: Logika tab non-rekursif
                U32 spaces_to_add = 4 - (g_cur_col % 4);
                for (U32 i = 0; i < spaces_to_add; ++i) {
                    if (g_cur_col >= g_cols) {
                        NewLine();
                    }
                    g_grid[g_cur_row * g_cols + g_cur_col] = ' ';
                    RenderCell(g_cur_col, g_cur_row);
                    ++g_cur_col;
                }
                continue;
            }
            // wrap if needed
            if (g_cur_col >= g_cols) {
                NewLine();
            }
            // store char and render cell
            g_grid[g_cur_row * g_cols + g_cur_col] = ch;
            RenderCell(g_cur_col, g_cur_row);
            ++g_cur_col;
        }

        ShowCursor();
    }

    VOID UpdateCursor(){
        if(!g_ready) return;

        U64 current_time = Arch::Time::NowTicks();
        U64 elapsed = current_time - g_last_blink_time;
        if(elapsed >= g_blink_interval_tick){
            if(g_cursor_visible){
                HideCursor();
            } else {
                ShowCursor();
            }
            g_last_blink_time = current_time;
        }
    }
}