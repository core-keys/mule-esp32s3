// core-keys device UI — a tiny anti-aliased 2D kit over an RGB565 framebuffer.
//
// Pure C, no ESP-IDF: the same code renders on the device (into the ST7789
// framebuffer) and on the host (into a buffer written out as an image), so the
// look can be iterated without flashing. The screen renderers (ui_screens.c)
// use only this.
#pragma once
#include <stdint.h>
#include <stdbool.h>

// ox/oy translate local coords into fb coords; cx0..cy1 clip in fb coords
// ([cx0,cx1) x [cy0,cy1), cx1 <= cx0 = no clip). Zeroed fields = plain canvas,
// so `{ fb, w, h }` initializers keep today's behavior.
typedef struct {
    uint16_t *fb; int w; int h;
    int ox, oy;
    int cx0, cy0, cx1, cy1;
} ui_canvas_t;

// Colors are native RGB565 (the panel's byte-swap is handled at flush time).
uint16_t ui_rgb(uint8_t r, uint8_t g, uint8_t b);
uint16_t ui_hex(uint32_t rgb);                 // 0xRRGGBB -> RGB565
uint16_t ui_mix(uint16_t a, uint16_t b, uint8_t t); // t=0..255 (0=a, 255=b)

void ui_clear(ui_canvas_t *cv, uint16_t color);   // fills the clip rect (viewport), ignores ox/oy
void ui_px(ui_canvas_t *cv, int x, int y, uint16_t color);
void ui_blend(ui_canvas_t *cv, int x, int y, uint16_t color, int a255); // over-blend
void ui_dim(ui_canvas_t *cv, int a255);           // darken the viewport toward black

void ui_rect(ui_canvas_t *cv, int x, int y, int w, int h, uint16_t color);
void ui_rrect(ui_canvas_t *cv, int x, int y, int w, int h, int r, uint16_t color);
void ui_rrect_border(ui_canvas_t *cv, int x, int y, int w, int h, int r, int th, uint16_t color);
void ui_hline(ui_canvas_t *cv, int x, int y, int w, uint16_t color);

// Blit a w x h block of RGB565 pixels (row-major) into the canvas at (x,y),
// clipped to canvas bounds. Opaque copy (no blending).
void ui_blit(ui_canvas_t *cv, int x, int y, int w, int h, const uint16_t *pixels);

// Anti-aliased ring / disc / arc / radial glow.
void ui_ring(ui_canvas_t *cv, int cx, int cy, float r, float th, uint16_t color);
void ui_disc(ui_canvas_t *cv, int cx, int cy, float r, uint16_t color);
void ui_arc(ui_canvas_t *cv, int cx, int cy, float r, float th,
            float a0, float a1, uint16_t color);            // radians, filled sweep
void ui_glow(ui_canvas_t *cv, int cx, int cy, float radius, uint16_t color, int max_a);

// Blit a packed 4-bpp alpha mask (w x h, high nibble = left pixel, row stride
// ceil(w/2) bytes) as `fg` over the canvas; a255 scales the whole mask's alpha.
// This is the glyph inner loop and also draws pre-rendered art (the wordmark).
void ui_amask(ui_canvas_t *cv, int x, int y, int w, int h,
              const uint8_t *a4, uint16_t fg, int a255);

// AA chevron stroke inside (x,y,w,h); dir 0=right,1=left,2=up,3=down.
void ui_chevron(ui_canvas_t *cv, int x, int y, int w, int h,
                int dir, float th, uint16_t color);

// ---- anti-aliased UI faces (tools/gen_font.py ui -> fonts.c) -----------------
// Glyphs are 4-bpp coverage generated at final pixel size — never scaled at
// runtime. `y` in the draw calls is the CELL TOP; `baseline` aligns faces.
typedef struct {
    uint8_t  first, last;        // ASCII range covered
    uint8_t  cell_h;             // storage rows per glyph
    uint8_t  baseline;           // rows from cell top to the baseline
    uint8_t  fixed_adv;          // advance when adv == NULL (monospace faces)
    const uint8_t  *adv;         // per-glyph advance px, or NULL
    const uint8_t  *gw;          // per-glyph ink width px
    const int8_t   *xoff;        // per-glyph pen -> ink x bearing
    const uint16_t *off;         // byte offset of each glyph's packed rows
    const uint8_t  *px;          // packed 4-bpp coverage, ceil(gw/2) bytes/row
} ui_font_t;

int  ui_ftext_w(const ui_font_t *f, const char *s);
int  ui_ftext(ui_canvas_t *cv, const ui_font_t *f, int x, int y,
              const char *s, uint16_t fg);                    // returns end x
void ui_ftext_center(ui_canvas_t *cv, const ui_font_t *f, int cx, int y,
                     const char *s, uint16_t fg);
// Letter-spaced variant (small-caps headers): +track px after every glyph.
int  ui_ftext_tracked_w(const ui_font_t *f, const char *s, int track);
void ui_ftext_tracked(ui_canvas_t *cv, const ui_font_t *f, int x, int y,
                      const char *s, int track, uint16_t fg);
// Truncate to max_px with a trailing "..".
void ui_ftext_fit(char *dst, int cap, const ui_font_t *f,
                  const char *src, int max_px);
// Truncate to max_px with a MIDDLE ".." keeping the tail — identity strings
// (rp ids, hostnames) must keep their registrable suffix visible, so a hostile
// "github.com.evil.example" can never truncate to "github.com..".
void ui_ftext_fit_tail(char *dst, int cap, const ui_font_t *f,
                       const char *src, int max_px);

// Legacy 8x16 bitmap font (sole remaining call site: the PAIR terminal command).
int  ui_text_w(const char *s, int scale);
void ui_text(ui_canvas_t *cv, int x, int y, const char *s, int scale, uint16_t fg);
void ui_text_center(ui_canvas_t *cv, int cx, int y, const char *s, int scale, uint16_t fg);
