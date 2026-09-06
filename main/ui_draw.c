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

// Viewport of a canvas in fb coords: the clip rect if active, else the whole fb.
static inline void viewport(const ui_canvas_t *cv, int *x0, int *y0, int *x1, int *y1)
{
    if (cv->cx1 > cv->cx0) { *x0 = cv->cx0; *y0 = cv->cy0; *x1 = cv->cx1; *y1 = cv->cy1; }
    else                   { *x0 = 0; *y0 = 0; *x1 = cv->w; *y1 = cv->h; }
}
static inline bool px_ok(const ui_canvas_t *cv, int x, int y)
{
    if ((unsigned)x >= (unsigned)cv->w || (unsigned)y >= (unsigned)cv->h) return false;
    if (cv->cx1 > cv->cx0 &&
        (x < cv->cx0 || x >= cv->cx1 || y < cv->cy0 || y >= cv->cy1)) return false;
    return true;
}

void ui_clear(ui_canvas_t *cv, uint16_t color)
{
    int x0, y0, x1, y1;
    viewport(cv, &x0, &y0, &x1, &y1);
    for (int j = y0; j < y1; j++) {
        uint16_t *row = &cv->fb[j * cv->w];
        for (int i = x0; i < x1; i++) row[i] = color;
    }
}
void ui_px(ui_canvas_t *cv, int x, int y, uint16_t color)
{
    x += cv->ox; y += cv->oy;
    if (px_ok(cv, x, y)) cv->fb[y * cv->w + x] = color;
}
void ui_blend(ui_canvas_t *cv, int x, int y, uint16_t color, int a)
{
    x += cv->ox; y += cv->oy;
    if (!px_ok(cv, x, y) || a <= 0) return;
    if (a >= 255) { cv->fb[y * cv->w + x] = color; return; }
    uint16_t *p = &cv->fb[y * cv->w + x];
    int sr, sg, sb, dr, dg, db;
    unpack(color, &sr, &sg, &sb); unpack(*p, &dr, &dg, &db);
    *p = ui_rgb((sr * a + dr * (255 - a)) / 255,
                (sg * a + dg * (255 - a)) / 255,
                (sb * a + db * (255 - a)) / 255);
}
void ui_dim(ui_canvas_t *cv, int a)
{
    if (a <= 0) return;
    if (a > 255) a = 255;
    int x0, y0, x1, y1;
    viewport(cv, &x0, &y0, &x1, &y1);
    uint16_t black = 0;
    for (int j = y0; j < y1; j++) {
        uint16_t *row = &cv->fb[j * cv->w];
        for (int i = x0; i < x1; i++) row[i] = ui_mix(row[i], black, a);
    }
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
void ui_blit(ui_canvas_t *cv, int x, int y, int w, int h, const uint16_t *pixels)
{
    x += cv->ox; y += cv->oy;
    for (int j = 0; j < h; j++) {
        int dy = y + j;
        const uint16_t *src = pixels + j * w;
        for (int i = 0; i < w; i++) {
            int dx = x + i;
            if (px_ok(cv, dx, dy)) cv->fb[dy * cv->w + dx] = src[i];
        }
    }
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

// ---- 4-bpp alpha masks / AA UI faces ----------------------------------------
void ui_amask(ui_canvas_t *cv, int x, int y, int w, int h,
              const uint8_t *a4, uint16_t fg, int a255)
{
    if (a255 <= 0) return;
    int stride = (w + 1) / 2;
    for (int j = 0; j < h; j++) {
        const uint8_t *row = a4 + j * stride;
        for (int i = 0; i < w; i++) {
            uint8_t b = row[i >> 1];
            int v = (i & 1) ? (b & 0x0F) : (b >> 4);
            if (!v) continue;
            int a = v * 17;
            if (a255 < 255) a = a * a255 / 255;
            ui_blend(cv, x + i, y + j, fg, a);
        }
    }
}

static int fglyph(const ui_font_t *f, char c)   // glyph index, or -1
{
    uint8_t u = (uint8_t)c;
    if (u >= f->first && u <= f->last) return u - f->first;
    if ('?' >= f->first && '?' <= f->last) return '?' - f->first;
    return -1;
}
static int fadv(const ui_font_t *f, int gi)
{
    if (gi < 0) return f->fixed_adv;
    return f->adv ? f->adv[gi] : f->fixed_adv;
}

int ui_ftext_w(const ui_font_t *f, const char *s)
{
    int w = 0;
    for (; *s; s++) w += fadv(f, fglyph(f, *s));
    return w;
}
int ui_ftext(ui_canvas_t *cv, const ui_font_t *f, int x, int y,
             const char *s, uint16_t fg)
{
    for (; *s; s++) {
        int gi = fglyph(f, *s);
        if (gi >= 0 && f->gw[gi])
            ui_amask(cv, x + f->xoff[gi], y, f->gw[gi], f->cell_h,
                     f->px + f->off[gi], fg, 255);
        x += fadv(f, gi);
    }
    return x;
}
void ui_ftext_center(ui_canvas_t *cv, const ui_font_t *f, int cx, int y,
                     const char *s, uint16_t fg)
{
    ui_ftext(cv, f, cx - ui_ftext_w(f, s) / 2, y, s, fg);
}
int ui_ftext_tracked_w(const ui_font_t *f, const char *s, int track)
{
    int n = (int)strlen(s);
    return ui_ftext_w(f, s) + (n > 1 ? (n - 1) * track : 0);
}
void ui_ftext_tracked(ui_canvas_t *cv, const ui_font_t *f, int x, int y,
                      const char *s, int track, uint16_t fg)
{
    for (; *s; s++) {
        int gi = fglyph(f, *s);
        if (gi >= 0 && f->gw[gi])
            ui_amask(cv, x + f->xoff[gi], y, f->gw[gi], f->cell_h,
                     f->px + f->off[gi], fg, 255);
        x += fadv(f, gi) + track;
    }
}

void ui_ftext_fit(char *dst, int cap, const ui_font_t *f,
                  const char *src, int max_px)
{
    if (cap <= 0) return;
    if (ui_ftext_w(f, src) <= max_px) {
        int n = (int)strlen(src);
        if (n > cap - 1) n = cap - 1;
        memcpy(dst, src, n); dst[n] = 0;
        return;
    }
    int dots = ui_ftext_w(f, "..");
    int w = 0, n = 0;
    for (; src[n] && n < cap - 3; n++) {
        int a = fadv(f, fglyph(f, src[n]));
        if (w + a + dots > max_px) break;
        w += a;
    }
    memcpy(dst, src, n);
    dst[n] = '.'; dst[n + 1] = '.'; dst[n + 2] = 0;
}

void ui_ftext_fit_tail(char *dst, int cap, const ui_font_t *f,
                       const char *src, int max_px)
{
    if (cap <= 0) return;
    int len = (int)strlen(src);
    if (ui_ftext_w(f, src) <= max_px && len <= cap - 1) {
        memcpy(dst, src, len); dst[len] = 0;
        return;
    }
    // Keep a 2-char head, spend everything else on the tail (the registrable
    // suffix is what the user must be able to verify).
    int head = len > 2 ? 2 : len;
    int avail = max_px - ui_ftext_w(f, "..");
    for (int i = 0; i < head; i++) avail -= fadv(f, fglyph(f, src[i]));
    int tw = 0, tail = 0;
    for (int i = len - 1; i > head; i--) {
        int a = fadv(f, fglyph(f, src[i]));
        if (tw + a > avail) break;
        tw += a; tail++;
    }
    if (head + 2 + tail > cap - 1) { tail = cap - 1 - head - 2; if (tail < 0) tail = 0; }
    memcpy(dst, src, head);
    dst[head] = '.'; dst[head + 1] = '.';
    memcpy(dst + head + 2, src + len - tail, tail);
    dst[head + 2 + tail] = 0;
}

// ---- chevron -----------------------------------------------------------------
static float seg_d(float px, float py, float ax, float ay, float bx, float by)
{
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float t = (vx * wx + vy * wy) / (vx * vx + vy * vy);
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    float dx = px - (ax + vx * t), dy = py - (ay + vy * t);
    return sqrtf(dx * dx + dy * dy);
}
void ui_chevron(ui_canvas_t *cv, int x, int y, int w, int h,
                int dir, float th, uint16_t color)
{
    float ax, ay, bx, by, cx, cy;   // two segments a->b, b->c; b is the apex
    switch (dir) {
    case 0:  ax = x;     ay = y;     bx = x + w; by = y + h * 0.5f; cx = x;     cy = y + h; break;
    case 1:  ax = x + w; ay = y;     bx = x;     by = y + h * 0.5f; cx = x + w; cy = y + h; break;
    case 2:  ax = x;     ay = y + h; bx = x + w * 0.5f; by = y;     cx = x + w; cy = y + h; break;
    default: ax = x;     ay = y;     bx = x + w * 0.5f; by = y + h; cx = x + w; cy = y;     break;
    }
    float half = th * 0.5f;
    int pad = (int)(half + 2);
    for (int j = y - pad; j <= y + h + pad; j++) {
        for (int i = x - pad; i <= x + w + pad; i++) {
            float px = i + 0.5f, py = j + 0.5f;
            float d1 = seg_d(px, py, ax, ay, bx, by);
            float d2 = seg_d(px, py, bx, by, cx, cy);
            float e = (d1 < d2 ? d1 : d2) - half;
            if (e <= 0) ui_px(cv, i, j, color);
            else if (e < 1.0f) ui_blend(cv, i, j, color, (int)((1.0f - e) * 255));
        }
    }
}

// ---- legacy 8x16 text ---------------------------------------------------------
int ui_text_w(const char *s, int scale) { return (int)strlen(s) * FONT_W * scale; }

static void draw_glyph(ui_canvas_t *cv, int x, int y, char c, int scale, uint16_t fg)
{
    if (c < FONT_FIRST || c > FONT_LAST) c = '?';
    const uint8_t *g = font8x16[(int)c - FONT_FIRST];
    // Integer block replication at every scale: crisp pixels, never resampled
    // (the old bilinear upscale left a grey halo around every stroke).
    for (int row = 0; row < FONT_H; row++) {
        uint8_t bits = g[row];
        if (!bits) continue;
        for (int col = 0; col < FONT_W; col++) {
            if (!(bits & (0x80 >> col))) continue;
            if (scale <= 1) { ui_px(cv, x + col, y + row, fg); continue; }
            for (int j = 0; j < scale; j++)
                for (int i = 0; i < scale; i++)
                    ui_px(cv, x + col * scale + i, y + row * scale + j, fg);
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

