// core-keys device UI — a tiny anti-aliased 2D kit over an RGB565 framebuffer.
//
// Pure C, no ESP-IDF: the same code renders on the device (into the ST7789
// framebuffer) and on the host (into a buffer written out as an image), so the
// look can be iterated without flashing. The screen renderers (ui_screens.c)
// use only this.
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct { uint16_t *fb; int w; int h; } ui_canvas_t;

// Colors are native RGB565 (the panel's byte-swap is handled at flush time).
uint16_t ui_rgb(uint8_t r, uint8_t g, uint8_t b);
uint16_t ui_hex(uint32_t rgb);                 // 0xRRGGBB -> RGB565
uint16_t ui_mix(uint16_t a, uint16_t b, uint8_t t); // t=0..255 (0=a, 255=b)

void ui_clear(ui_canvas_t *cv, uint16_t color);
void ui_px(ui_canvas_t *cv, int x, int y, uint16_t color);
void ui_blend(ui_canvas_t *cv, int x, int y, uint16_t color, int a255); // over-blend

void ui_rect(ui_canvas_t *cv, int x, int y, int w, int h, uint16_t color);
void ui_rrect(ui_canvas_t *cv, int x, int y, int w, int h, int r, uint16_t color);
void ui_rrect_border(ui_canvas_t *cv, int x, int y, int w, int h, int r, int th, uint16_t color);
void ui_hline(ui_canvas_t *cv, int x, int y, int w, uint16_t color);

// Anti-aliased ring / disc / arc / radial glow.
void ui_ring(ui_canvas_t *cv, int cx, int cy, float r, float th, uint16_t color);
void ui_disc(ui_canvas_t *cv, int cx, int cy, float r, uint16_t color);
void ui_arc(ui_canvas_t *cv, int cx, int cy, float r, float th,
            float a0, float a1, uint16_t color);            // radians, filled sweep
void ui_glow(ui_canvas_t *cv, int cx, int cy, float radius, uint16_t color, int max_a);

// Text (scaled 8x16 bitmap font). ui_text draws only lit pixels (transparent bg).
int  ui_text_w(const char *s, int scale);
void ui_text(ui_canvas_t *cv, int x, int y, const char *s, int scale, uint16_t fg);
void ui_text_center(ui_canvas_t *cv, int cx, int y, const char *s, int scale, uint16_t fg);
