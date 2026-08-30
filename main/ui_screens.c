// core-keys device screens. See ui_screens.h.
#include "ui_screens.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

#define W 170
#define H 320
#define CX 85
#define TAU 6.28318531f

static int toupper_(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static float lerp(float a, float b, float t) { return a + (b - a) * t; }
static float ease_out(float p) { p = clampf(p, 0, 1); return 1 - (1 - p) * (1 - p); }

// ---- the mark: two interlocking rings + a core at their overlap -------------
// agent ring (top, cool/light) and person ring (bottom, indigo); core lights
// only when `lit`. `sep` is the half-distance between ring centres.
static void draw_rings(ui_canvas_t *cv, int cx, int cy, float r, float th,
                       float sep, bool lit, float core_scale)
{
    int cyt = cy - (int)sep, cyb = cy + (int)sep;
    // Draw person (bottom) behind, agent (top) over it — so the RIGHT crossing
    // has the agent in front. Then redraw a short arc of the person ring at the
    // LEFT crossing so it passes in front there — that alternation is what makes
    // the two read as a linked chain, not two stacked circles.
    ui_ring(cv, cx, cyb, r, th, ui_hex(UIC_RING_P));
    ui_ring(cv, cx, cyt, r, th, ui_hex(UIC_RING_A));
    if ((float)sep < r) {
        float d = sqrtf(r * r - (float)sep * (float)sep);     // half-distance between crossings
        float ang = atan2f((float)(cy - cyb), -d);            // person-ring angle to the left crossing
        ui_arc(cv, cx, cyb, r, th, ang - 0.42f, ang + 0.42f, ui_hex(UIC_RING_P));
    }

    if (lit) {
        ui_glow(cv, cx, cy, r * 1.15f * core_scale, ui_hex(UIC_CORE_B), 150);
        ui_disc(cv, cx, cy, th * 0.62f * core_scale, ui_hex(UIC_CORE_G));
        ui_disc(cv, cx, cy, th * 0.34f * core_scale, ui_hex(UIC_SPARK));
    } else {
        ui_disc(cv, cx, cy, th * 0.34f, ui_hex(UIC_LINE2));
    }
}

// small status chip, right-aligned pill
static void chip(ui_canvas_t *cv, int right, int y, const char *s, uint16_t fg, uint16_t border)
{
    int tw = ui_text_w(s, 1);
    int w = tw + 14, x = right - w;
    ui_rrect_border(cv, x, y, w, 16, 8, 1, border);
    ui_text(cv, x + 7, y + 4, s, 1, fg);
}

// top status strip
static void topbar(ui_canvas_t *cv, const char *left, const char *right, uint16_t rcol)
{
    ui_text(cv, 12, 12, left, 1, ui_hex(UIC_MUTED));
    if (right) ui_text(cv, W - 12 - ui_text_w(right, 1), 12, right, 1, rcol);
}

// ---- BOOT --------------------------------------------------------------------
static void scr_boot(ui_canvas_t *cv, const ui_state_t *st)
{
    ui_clear(cv, ui_hex(UIC_BG));
    float p = ease_out(st->t / 1.0f);
    float sep = lerp(74, 30, p);          // rings glide together
    bool lit = st->t > 0.70f;
    float cs = lit ? clampf((st->t - 0.70f) / 0.35f, 0.2f, 1.0f) : 0;
    draw_rings(cv, CX, 128, 40, 13, sep, lit, cs > 0 ? cs : 1);

    if (st->t > 1.0f) {
        int a = (int)(clampf((st->t - 1.0f) / 0.4f, 0, 1) * 255);
        ui_text_center(cv, CX, 214, "core-keys", 2, ui_mix(ui_hex(UIC_BG), ui_hex(UIC_INK), a));
        ui_text_center(cv, CX, 244, "SPLIT-KEY", 1, ui_mix(ui_hex(UIC_BG), ui_hex(UIC_CORE_G), a));
    }
}

// ---- LOCK / PIN --------------------------------------------------------------
static void scr_lock(ui_canvas_t *cv, const ui_state_t *st)
{
    ui_clear(cv, ui_hex(UIC_BG));
    topbar(cv, "LOCKED", NULL, 0);

    // small "locked" rings — held apart, core dark
    draw_rings(cv, CX, 60, 22, 8, 22, false, 0);

    ui_text_center(cv, CX, 108, "ENTER PIN", 1, ui_hex(UIC_MUTED));

    // progress dots
    int n = st->pin_max ? st->pin_max : 4;
    int gap = 18, x0 = CX - (n - 1) * gap / 2;
    for (int i = 0; i < n; i++) {
        bool on = i < st->pin_len;
        if (on) { ui_glow(cv, x0 + i * gap, 132, 9, ui_hex(UIC_CORE_G), 120); ui_disc(cv, x0 + i * gap, 132, 4, ui_hex(UIC_CORE_G)); }
        else ui_ring(cv, x0 + i * gap, 132, 4, 1.5f, ui_hex(UIC_LINE2));
    }

    // the dial: current digit, large
    char d[2] = { (char)('0' + (st->pin_digit % 10)), 0 };
    uint16_t dc = st->pin_wrong ? ui_hex(UIC_DANGER) : ui_hex(UIC_CORE_G);
    ui_glow(cv, CX, 200, 40, dc, st->pin_wrong ? 90 : 70);
    ui_text_center(cv, CX, 170, d, 6, dc);

    ui_hline(cv, 14, 280, W - 28, ui_hex(UIC_LINE));
    ui_text_center(cv, CX, 290, "IO14 +1   BOOT next", 1, ui_hex(UIC_MUTED));
}

// ---- HOME --------------------------------------------------------------------
static void scr_home(ui_canvas_t *cv, const ui_state_t *st)
{
    ui_clear(cv, ui_hex(UIC_BG));
    topbar(cv, "core-keys", NULL, 0);
    if (st->paired) chip(cv, W - 12, 8, "PAIRED", ui_hex(UIC_OK), ui_rgb(0x2e, 0x6d, 0x58));
    else chip(cv, W - 12, 8, "UNPAIRED", ui_hex(UIC_MUTED), ui_hex(UIC_LINE2));

    // breathing core
    float br = 1.0f + 0.12f * sinf(st->t * 1.8f);
    draw_rings(cv, CX, 128, 40, 13, 30, st->unlocked, br);

    ui_text_center(cv, CX, 196, "core-keys", 2, ui_hex(UIC_INK));
    ui_text_center(cv, CX, 224, st->unlocked ? "ready . unlocked" : "locked", 1,
                   st->unlocked ? ui_hex(UIC_MUTED) : ui_hex(UIC_DANGER));

    // footer stats
    ui_hline(cv, 14, 286, W - 28, ui_hex(UIC_LINE));
    char l[16], rr[16];
    snprintf(l, sizeof(l), "%d creds", st->cred_count);
    snprintf(rr, sizeof(rr), "%d desktop", st->daemon_count);
    ui_text(cv, 14, 296, l, 1, ui_hex(UIC_SUB));
    ui_text(cv, W - 14 - ui_text_w(rr, 1), 296, rr, 1, ui_hex(UIC_SUB));
}

// ---- CREDENTIALS -------------------------------------------------------------
static void scr_creds(ui_canvas_t *cv, const ui_state_t *st)
{
    ui_clear(cv, ui_hex(UIC_BG));
    char cnt[8]; snprintf(cnt, sizeof(cnt), "%d", st->cred_n);
    topbar(cv, "CREDENTIALS", cnt, ui_hex(UIC_MUTED));

    int y = 34, rowh = 40, top = st->cred_top;
    for (int i = top; i < st->cred_n && i < top + 6; i++) {
        bool sel = (i == st->cred_sel);
        int ry = y + (i - top) * rowh;
        if (sel) {
            ui_rrect(cv, 10, ry, W - 20, rowh - 6, 9, ui_hex(0x1b2450));
            ui_rrect_border(cv, 10, ry, W - 20, rowh - 6, 9, 1, ui_hex(UIC_CORE_B));
        }
        // favicon tile — first two letters
        char fi[3] = { (char)toupper_((int)st->cred_rp[i][0]), (char)toupper_((int)st->cred_rp[i][1]), 0 };
        ui_rrect(cv, 18, ry + 6, 22, 22, 6, ui_hex(UIC_CARD));
        ui_text(cv, 24, ry + 10, fi, 1, ui_hex(UIC_CORE_G));
        ui_text(cv, 50, ry + 7, st->cred_rp[i], 1, ui_hex(UIC_INK));
        ui_text(cv, 50, ry + 21, st->cred_user[i], 1, ui_hex(UIC_MUTED));
    }
    ui_hline(cv, 14, 292, W - 28, ui_hex(UIC_LINE));
    ui_text_center(cv, CX, 300, "IO14 down  BOOT open", 1, ui_hex(UIC_MUTED));
}

// ---- APPROVAL ----------------------------------------------------------------
static void scr_approve(ui_canvas_t *cv, const ui_state_t *st)
{
    ui_clear(cv, ui_hex(UIC_BG));
    topbar(cv, "SIGN IN", "FIDO2", ui_hex(UIC_CORE_G));

    int cy = 132;
    // progress ring around the mark
    ui_ring(cv, CX, cy, 52, 6, ui_hex(UIC_LINE2));
    float p = clampf(st->progress, 0, 1);
    if (p > 0) ui_arc(cv, CX, cy, 52, 6, -TAU * 0.25f, -TAU * 0.25f + TAU * p, ui_hex(UIC_OK));
    draw_rings(cv, CX, cy, 30, 10, 24, true, 1);

    ui_text_center(cv, CX, 200, st->rp ? st->rp : "?", 2, ui_hex(UIC_INK));
    if (st->coauthd)
        ui_text_center(cv, CX, 230, "desktop co-authorized", 1, ui_hex(UIC_OK));
    else
        ui_text_center(cv, CX, 230, "waiting for desktop", 1, ui_hex(UIC_MUTED));

    ui_hline(cv, 14, 284, W - 28, ui_hex(UIC_LINE));
    ui_text_center(cv, CX, 294, "HOLD BOOT TO SIGN", 1, ui_hex(UIC_INK));
}

// ---- PAIRING -----------------------------------------------------------------
static void scr_pair(ui_canvas_t *cv, const ui_state_t *st)
{
    ui_clear(cv, ui_hex(UIC_BG));
    topbar(cv, "PAIRING", "SAS", ui_hex(UIC_CORE_G));
    draw_rings(cv, CX, 74, 26, 9, 20, true, 1);   // linking
    ui_text_center(cv, CX, 128, "COMPARE", 1, ui_hex(UIC_MUTED));
    ui_text_center(cv, CX, 154, st->sas ? st->sas : "------", 3, ui_hex(UIC_INK));
    ui_text_center(cv, CX, 214, "machine . unverified", 1, ui_hex(UIC_MUTED));
    ui_text_center(cv, CX, 230, st->machine ? st->machine : "?", 1, ui_hex(UIC_CORE_G));
    ui_hline(cv, 14, 284, W - 28, ui_hex(UIC_LINE));
    ui_text_center(cv, CX, 294, "match? press BOOT", 1, ui_hex(UIC_SUB));
}

void ui_render(ui_canvas_t *cv, const ui_state_t *st)
{
    switch (st->screen) {
    case UI_BOOT:    scr_boot(cv, st); break;
    case UI_LOCK:    scr_lock(cv, st); break;
    case UI_HOME:    scr_home(cv, st); break;
    case UI_CREDS:   scr_creds(cv, st); break;
    case UI_APPROVE: scr_approve(cv, st); break;
    case UI_PAIR:    scr_pair(cv, st); break;
    }
}
