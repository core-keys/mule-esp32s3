// core-keys device UI drawing kit. See ui_draw.h.
#include "ui_draw.h"
#include "font8x16.h"
#include <math.h>
#include <string.h>

uint16_t ui_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
uint16_t ui_hex(uint32_t rgb)
{
    return ui_rgb((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

static inline void unpack(uint16_t c, int *r, int *g, int *b)
{
    *r = ((c >> 11) & 0x1F) << 3;
    *g = ((c >> 5) & 0x3F) << 2;
    *b = (c & 0x1F) << 3;
}
uint16_t ui_mix(uint16_t a, uint16_t b, uint8_t t)
{
    int ar, ag, ab, br, bg, bb;
    unpack(a, &ar, &ag, &ab); unpack(b, &br, &bg, &bb);
    return ui_rgb((ar * (255 - t) + br * t) / 255,
                  (ag * (255 - t) + bg * t) / 255,
                  (ab * (255 - t) + bb * t) / 255);
}

void ui_clear(ui_canvas_t *cv, uint16_t color)
{
    int n = cv->w * cv->h;
    for (int i = 0; i < n; i++) cv->fb[i] = color;
}
void ui_px(ui_canvas_t *cv, int x, int y, uint16_t color)
{
    if ((unsigned)x < (unsigned)cv->w && (unsigned)y < (unsigned)cv->h)
        cv->fb[y * cv->w + x] = color;
}
void ui_blend(ui_canvas_t *cv, int x, int y, uint16_t color, int a)
{
    if ((unsigned)x >= (unsigned)cv->w || (unsigned)y >= (unsigned)cv->h || a <= 0) return;
    if (a >= 255) { cv->fb[y * cv->w + x] = color; return; }
    uint16_t *p = &cv->fb[y * cv->w + x];
    int sr, sg, sb, dr, dg, db;
    unpack(color, &sr, &sg, &sb); unpack(*p, &dr, &dg, &db);
    *p = ui_rgb((sr * a + dr * (255 - a)) / 255,
                (sg * a + dg * (255 - a)) / 255,
                (sb * a + db * (255 - a)) / 255);
}

void ui_rect(ui_canvas_t *cv, int x, int y, int w, int h, uint16_t color)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) ui_px(cv, i, j, color);
}
void ui_hline(ui_canvas_t *cv, int x, int y, int w, uint16_t color)
{
    for (int i = x; i < x + w; i++) ui_px(cv, i, y, color);
}

// Coverage of a rounded corner centered at (ccx,ccy) with radius r, at pixel px,py.
static int corner_cov(float px, float py, float ccx, float ccy, float r)
{
    float d = sqrtf((px - ccx) * (px - ccx) + (py - ccy) * (py - ccy));
    float e = d - r;                 // >0 outside
    if (e <= 0) return 255;
    if (e < 1.0f) return (int)((1.0f - e) * 255);
    return 0;
}
void ui_rrect(ui_canvas_t *cv, int x, int y, int w, int h, int r, uint16_t color)
{
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int a = 255;
            float px = i + 0.5f, py = j + 0.5f;
            if (i < r && j < r) a = corner_cov(px, py, r, r, r);
            else if (i >= w - r && j < r) a = corner_cov(px, py, w - r, r, r);
            else if (i < r && j >= h - r) a = corner_cov(px, py, r, h - r, r);
            else if (i >= w - r && j >= h - r) a = corner_cov(px, py, w - r, h - r, r);
            if (a) ui_blend(cv, x + i, y + j, color, a);
        }
    }
}
void ui_rrect_border(ui_canvas_t *cv, int x, int y, int w, int h, int r, int th, uint16_t color)
{
    // Cheap: draw filled rrect, then punch the inner rrect with... simpler to
    // stroke edges + corner arcs. Draw as difference via per-pixel ring test.
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            float px = i + 0.5f, py = j + 0.5f;
            // distance to the rounded-rect boundary (approx via corner discs + edges)
            float d;
            if (i < r && j < r) d = sqrtf((px - r) * (px - r) + (py - r) * (py - r)) - r;
            else if (i >= w - r && j < r) d = sqrtf((px - (w - r)) * (px - (w - r)) + (py - r) * (py - r)) - r;
            else if (i < r && j >= h - r) d = sqrtf((px - r) * (px - r) + (py - (h - r)) * (py - (h - r))) - r;
            else if (i >= w - r && j >= h - r) d = sqrtf((px - (w - r)) * (px - (w - r)) + (py - (h - r)) * (py - (h - r))) - r;
            else {
                float dx = fminf(px, w - px), dy = fminf(py, h - py);
                d = -fminf(dx, dy);
            }
            float e = fabsf(d + th * 0.5f) - th * 0.5f; // band of width th just inside the edge
            int a = 0;
            if (e <= 0) a = 255; else if (e < 1.0f) a = (int)((1.0f - e) * 255);
            if (a) ui_blend(cv, x + i, y + j, color, a);
        }
    }
}

void ui_ring(ui_canvas_t *cv, int cx, int cy, float r, float th, uint16_t color)
{
    float half = th * 0.5f;
    int rmax = (int)(r + half + 2);
    for (int y = cy - rmax; y <= cy + rmax; y++) {
        for (int x = cx - rmax; x <= cx + rmax; x++) {
            float dx = x - cx, dy = y - cy;
            float d = sqrtf(dx * dx + dy * dy);
            float e = fabsf(d - r) - half;
            if (e <= 0) ui_px(cv, x, y, color);
            else if (e < 1.0f) ui_blend(cv, x, y, color, (int)((1.0f - e) * 255));
        }
    }
}
void ui_disc(ui_canvas_t *cv, int cx, int cy, float r, uint16_t color)
{
    int rmax = (int)(r + 2);
    for (int y = cy - rmax; y <= cy + rmax; y++) {
        for (int x = cx - rmax; x <= cx + rmax; x++) {
            float dx = x - cx, dy = y - cy;
            float e = sqrtf(dx * dx + dy * dy) - r;
            if (e <= 0) ui_px(cv, x, y, color);
            else if (e < 1.0f) ui_blend(cv, x, y, color, (int)((1.0f - e) * 255));
        }
    }
}
void ui_arc(ui_canvas_t *cv, int cx, int cy, float r, float th,
            float a0, float a1, uint16_t color)
{
    float half = th * 0.5f;
    int rmax = (int)(r + half + 2);
    for (int y = cy - rmax; y <= cy + rmax; y++) {
        for (int x = cx - rmax; x <= cx + rmax; x++) {
            float dx = x - cx, dy = y - cy;
            float d = sqrtf(dx * dx + dy * dy);
            float e = fabsf(d - r) - half;
            if (e >= 1.0f) continue;
            float ang = atan2f(dy, dx);
            if (ang < 0) ang += 6.28318531f;
            float aa = a0, bb = a1;
            if (aa < 0) aa += 6.28318531f;
            if (bb < 0) bb += 6.28318531f;
            bool in;
            if (aa <= bb) in = (ang >= aa && ang <= bb);
            else in = (ang >= aa || ang <= bb);
            if (!in) continue;
            int cov = (e <= 0) ? 255 : (int)((1.0f - e) * 255);
            ui_blend(cv, x, y, color, cov);
        }
    }
}
void ui_glow(ui_canvas_t *cv, int cx, int cy, float R, uint16_t color, int max_a)
{
    int rmax = (int)(R + 1);
    for (int y = cy - rmax; y <= cy + rmax; y++) {
        for (int x = cx - rmax; x <= cx + rmax; x++) {
            float dx = x - cx, dy = y - cy;
            float d = sqrtf(dx * dx + dy * dy);
            if (d >= R) continue;
            float t = 1.0f - d / R;
            int a = (int)(t * t * max_a);
            if (a) ui_blend(cv, x, y, color, a);
        }
    }
}

// ---- text --------------------------------------------------------------------
int ui_text_w(const char *s, int scale) { return (int)strlen(s) * FONT_W * scale; }

static void draw_glyph(ui_canvas_t *cv, int x, int y, char c, int scale, uint16_t fg)
{
    if (c < FONT_FIRST || c > FONT_LAST) c = '?';
    const uint8_t *g = font8x16[(int)c - FONT_FIRST];
    for (int row = 0; row < FONT_H; row++) {
        uint8_t bits = g[row];
        if (!bits) continue;
        for (int col = 0; col < FONT_W; col++) {
            if (!(bits & (0x80 >> col))) continue;
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++)
                    ui_px(cv, x + col * scale + sx, y + row * scale + sy, fg);
        }
    }
}
void ui_text(ui_canvas_t *cv, int x, int y, const char *s, int scale, uint16_t fg)
{
    for (; *s; s++) { draw_glyph(cv, x, y, *s, scale, fg); x += FONT_W * scale; }
}
void ui_text_center(ui_canvas_t *cv, int cx, int y, const char *s, int scale, uint16_t fg)
{
    ui_text(cv, cx - ui_text_w(s, scale) / 2, y, s, scale, fg);
}
