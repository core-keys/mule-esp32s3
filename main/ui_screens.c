// core-keys device screens. See ui_screens.h.
#include "ui_screens.h"
#include "fonts.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

#define W 170
#define H 320
#define CX 85
#define TAU 6.28318531f

#define F_SMALL (&ui_font_small)
#define F_BODY  (&ui_font_body)
#define F_TITLE (&ui_font_title)
#define F_MONO  (&ui_font_mono)
#define F_SAS   (&ui_font_sas)

// ---- shared touch geometry (renderers + ui_hit_test) ------------------------
#define KP_W 52
#define KP_H 46
#define KP_GX 4
#define KP_GY 6
#define KP_X0 3              // (170 - (3*52 + 2*4)) / 2
#define KP_Y0 100
static const char *KP_LABELS[4][3] = {
    { "1", "2", "3" }, { "4", "5", "6" }, { "7", "8", "9" }, { "C", "0", "" }
};
// primary action button (APPROVE / MATCH): the largest target in the system
#define APPR_BTN_X 12
#define APPR_BTN_W 146
#define APPR_BTN_H 60
#define APPR_BTN_Y 238
// home: the tappable "N KEYS · N DESKTOP" meta line band
#define HOME_META_Y0 202
#define HOME_META_Y1 250
// credentials: DELETE bar under the list; confirm-dialog targets
#define CRED_DEL_X 12
#define CRED_DEL_W 146
#define CRED_DEL_H 56
#define CRED_DEL_Y 240
#define CONF_DEL_Y 200       // confirm dialog: DELETE button top
#define CONF_CANCEL_Y0 258   // confirm dialog: CANCEL text-button band
#define CONF_CANCEL_Y1 302

static int toupper_(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static float lerp(float a, float b, float t) { return a + (b - a) * t; }
static float ease_out(float p) { p = clampf(p, 0, 1); return 1 - (1 - p) * (1 - p); }

// Small-caps header idiom: F_SMALL, +2px tracking, muted unless overridden.
static void smallcaps(ui_canvas_t *cv, int x, int y, const char *s, uint16_t c)
{
    ui_ftext_tracked(cv, F_SMALL, x, y, s, 2, c);
}
static void smallcaps_center(ui_canvas_t *cv, int cx, int y, const char *s, uint16_t c)
{
    ui_ftext_tracked(cv, F_SMALL, cx - ui_ftext_tracked_w(F_SMALL, s, 2) / 2, y, s, 2, c);
}

// A translated/clipped view of a canvas (clip in fb coords; cx1 <= cx0 = none).
static ui_canvas_t ui_view(const ui_canvas_t *b, int ox, int oy,
                           int cx0, int cy0, int cx1, int cy1)
{
    ui_canvas_t v = *b;
    v.ox += ox; v.oy += oy;
    v.cx0 = cx0; v.cy0 = cy0; v.cx1 = cx1; v.cy1 = cy1;
    return v;
}

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

// A horizontal battery gauge. pct 0..100 fills + tints by level; pct < 0 is
// "no cell / unknown" and draws a dashed placeholder (battery hardware is TBD).
#define BATT_W 30
#define BATT_H 15
static uint16_t batt_color(int p) {
    return p <= 10 ? ui_hex(UIC_DANGER) : p <= 30 ? ui_hex(UIC_WARN) : ui_hex(UIC_OK);
}
// A small lightning bolt (5x7), for the charging state.
static void draw_bolt(ui_canvas_t *cv, int x, int y, uint16_t c)
{
    static const uint8_t b[7] = { 0x06, 0x0C, 0x18, 0x1E, 0x06, 0x0C, 0x18 };
    for (int r = 0; r < 7; r++)
        for (int col = 0; col < 5; col++)
            if (b[r] & (1 << (4 - col))) ui_px(cv, x + col, y + r, c);
}
static void battery(ui_canvas_t *cv, int x, int y, int pct, bool charging)
{
    ui_rrect_border(cv, x, y, BATT_W, BATT_H, 3, 1, ui_hex(UIC_SUB));
    ui_rect(cv, x + BATT_W, y + 5, 2, BATT_H - 10, ui_hex(UIC_SUB));     // terminal nub
    if (pct < 0) {                                                       // unknown: dashes
        for (int i = 0; i < 3; i++) ui_rect(cv, x + 7 + i * 6, y + BATT_H / 2 - 1, 3, 2, ui_hex(UIC_MUTED));
    } else {
        int p = pct > 100 ? 100 : pct;
        int fill = (BATT_W - 5) * p / 100;
        if (fill > 0) ui_rect(cv, x + 2, y + 2, fill, BATT_H - 4, batt_color(p));
    }
    if (charging)                                                        // bolt overlays the fill
        draw_bolt(cv, x + BATT_W / 2 - 2, y + (BATT_H - 7) / 2, ui_hex(UIC_INK));
}

// Paired indicator: two small interlocking rings (the brand mark), lit when paired.
static void link_glyph(ui_canvas_t *cv, int cx, int cy, bool paired)
{
    uint16_t c = paired ? ui_hex(UIC_OK) : ui_hex(UIC_MUTED);
    ui_ring(cv, cx - 3, cy, 3.4f, 1.5f, c);
    ui_ring(cv, cx + 3, cy, 3.4f, 1.5f, c);
}

// The persistent top status cluster (y 0..31): link glyph at left, %-charge +
// battery gauge at right, hairline below. Drawn by HOME and, sheet-local, by
// the curtain — one helper so the two can never diverge.
static void status_bar(ui_canvas_t *cv, bool paired, int batt_pct, bool charging)
{
    link_glyph(cv, 18, 19, paired);
    int x_bat = 128;
    battery(cv, x_bat, 11, batt_pct, charging);
    if (batt_pct >= 0) {
        int p = batt_pct > 100 ? 100 : batt_pct;
        char pc[6]; snprintf(pc, sizeof pc, "%d%%", p);
        ui_ftext(cv, F_SMALL, x_bat - 6 - ui_ftext_w(F_SMALL, pc), 10, pc, batt_color(p));
    }
    ui_hline(cv, 12, 31, W - 24, ui_hex(UIC_LINE));
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
        ui_amask(cv, (W - UI_WORDMARK_W) / 2, 204, UI_WORDMARK_W, UI_WORDMARK_H,
                 img_wordmark, ui_hex(UIC_INK), a);
        uint16_t tc = ui_mix(ui_hex(UIC_BG), ui_hex(UIC_CORE_G), a);
        ui_ftext_tracked(cv, F_SMALL,
                         CX - ui_ftext_tracked_w(F_SMALL, "SPLIT-KEY", 3) / 2, 248,
                         "SPLIT-KEY", 3, tc);
    }
}

// ---- LOCK / PIN --------------------------------------------------------------
static void scr_lock(ui_canvas_t *cv, const ui_state_t *st)
{
    ui_clear(cv, ui_hex(UIC_BG));
    const char *title = st->lock_mode == 1 ? "SET A PIN"
                      : st->lock_mode == 2 ? "CONFIRM PIN" : "ENTER PIN";
    ui_ftext_center(cv, F_TITLE, CX, 14, title, ui_hex(UIC_CORE_G));

    // progress dots
    int n = st->pin_max ? st->pin_max : 4;
    int gap = 26, x0 = CX - (n - 1) * gap / 2;
    for (int i = 0; i < n; i++) {
        if (i < st->pin_len) { ui_glow(cv, x0 + i * gap, 58, 11, ui_hex(UIC_CORE_G), 120); ui_disc(cv, x0 + i * gap, 58, 6, ui_hex(UIC_CORE_G)); }
        else ui_ring(cv, x0 + i * gap, 58, 6, 1.5f, ui_hex(UIC_LINE2));
    }
    if (st->pin_wrong && st->lock_mode == 0) {
        char m[24]; snprintf(m, sizeof(m), "wrong - %d left", st->pin_retries);
        ui_ftext_center(cv, F_SMALL, CX, 78, m, ui_hex(UIC_DANGER));
    } else if (st->pin_wrong && st->lock_mode == 2) {
        ui_ftext_center(cv, F_SMALL, CX, 78, "did not match", ui_hex(UIC_DANGER));
    }

    // numeric keypad (tap); the two buttons still dial+commit in parallel.
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 3; c++) {
            int x = KP_X0 + c * (KP_W + KP_GX), y = KP_Y0 + r * (KP_H + KP_GY);
            ui_rrect(cv, x, y, KP_W, KP_H, 10, ui_hex(UIC_CARD));
            ui_rrect_border(cv, x, y, KP_W, KP_H, 10, 1, ui_hex(UIC_LINE2));
            if (r == 3 && c == 2) {                    // backspace: chevron, not '<'
                ui_chevron(cv, x + (KP_W - 10) / 2, y + (KP_H - 14) / 2, 10, 14,
                           1, 2.0f, ui_hex(UIC_MUTED));
                continue;
            }
            uint16_t lc = (r == 3 && c == 0) ? ui_hex(UIC_MUTED) : ui_hex(UIC_INK);
            ui_ftext_center(cv, F_TITLE, x + KP_W / 2, y + (KP_H - F_TITLE->cell_h) / 2, KP_LABELS[r][c], lc);
        }
    }
}

// ---- HOME --------------------------------------------------------------------
static void scr_home(ui_canvas_t *cv, const ui_state_t *st)
{
    ui_clear(cv, ui_hex(UIC_BG));
    status_bar(cv, st->paired, st->batt_pct, st->batt_charging);
    // curtain grabber at the top edge; brightens as soon as a pull starts
    bool pulling = st->menu_prog > 0.003f;
    ui_rrect(cv, 69, 3, 32, 4, 2, ui_hex(pulling ? UIC_MUTED : UIC_LINE2));

    float br = 1.0f + 0.10f * sinf(st->t * 1.8f);
    draw_rings(cv, CX, 110, 34, 11, 25, st->unlocked, br);
    ui_amask(cv, (W - UI_WORDMARK_W) / 2, 164, UI_WORDMARK_W, UI_WORDMARK_H,
             img_wordmark, ui_hex(UIC_INK), 255);

    // One tappable meta line: "N KEYS · N DESKTOP" (band = HOME_META_Y0..Y1).
    char k[20], d[24];
    if (st->cred_count == 1) snprintf(k, sizeof k, "1 KEY");
    else if (st->cred_count > 0) snprintf(k, sizeof k, "%d KEYS", st->cred_count);
    else snprintf(k, sizeof k, "NO KEYS");
    bool unpaired = st->daemon_count == 0;
    if (unpaired) snprintf(d, sizeof d, "UNPAIRED");
    else if (st->daemon_count == 1) snprintf(d, sizeof d, "1 DESKTOP");
    else snprintf(d, sizeof d, "%d DESKTOPS", st->daemon_count);
    int w1 = ui_ftext_w(F_SMALL, k), w2 = ui_ftext_w(F_SMALL, d);
    int x = CX - (w1 + 8 + 3 + 8 + w2) / 2;
    ui_ftext(cv, F_SMALL, x, 224, k, ui_hex(UIC_SUB));
    ui_disc(cv, x + w1 + 9, 233, 1.5f, ui_hex(UIC_LINE2));
    ui_ftext(cv, F_SMALL, x + w1 + 19, 224, d, ui_hex(unpaired ? UIC_WARN : UIC_MUTED));

    // page dots: home is the left page, credentials the right
    ui_disc(cv, 79, 306, 2.5f, ui_hex(UIC_SUB));
    ui_ring(cv, 91, 306, 2.5f, 1.2f, ui_hex(UIC_LINE2));
}

// A filled primary action button (tap target), fill + border colored.
static void action_btn_c(ui_canvas_t *cv, int x, int y, int w, int h, const char *label,
                         uint16_t fill, uint16_t border, uint16_t ink)
{
    ui_rrect(cv, x, y, w, h, 12, fill);
    ui_rrect_border(cv, x, y, w, h, 12, 2, border);
    ui_ftext_center(cv, F_TITLE, x + w / 2, y + (h - F_TITLE->cell_h) / 2 + 1, label, ink);
}
// The primary (indigo) action button.
static void action_btn(ui_canvas_t *cv, int x, int y, int w, int h, const char *label)
{
    action_btn_c(cv, x, y, w, h, label, ui_hex(UIC_BTN), ui_hex(UIC_CORE_B), ui_hex(UIC_INK));
}

// ---- CREDENTIALS -------------------------------------------------------------
static void scr_creds(ui_canvas_t *cv, const ui_state_t *st)
{
    ui_clear(cv, ui_hex(UIC_BG));

    // Confirm-delete dialog: name the exact site being removed.
    if (st->confirm_del) {
        bool have = st->cred_sel >= 0 && st->cred_sel < st->cred_n;
        const char *rp = have ? st->cred_rp[st->cred_sel] : "";
        const char *us = have ? st->cred_user[st->cred_sel] : "";
        ui_ftext_center(cv, F_TITLE, CX, 20, "REMOVE KEY?", ui_hex(UIC_INK));
        ui_rrect(cv, 12, 88, W - 24, 88, 12, ui_hex(UIC_CARD));
        char drp[24], dus[28];
        ui_ftext_fit_tail(drp, sizeof drp, F_MONO, rp, 130);   // identity: tail preserved
        ui_ftext_fit(dus, sizeof dus, F_SMALL, us, 130);
        ui_ftext_center(cv, F_MONO, CX, 102, drp, ui_hex(UIC_INK));
        ui_ftext_center(cv, F_SMALL, CX, 136, dus, ui_hex(UIC_MUTED));
        action_btn_c(cv, CRED_DEL_X, CONF_DEL_Y, CRED_DEL_W, CRED_DEL_H, "DELETE",
                     ui_hex(UIC_DANGER_BG), ui_hex(UIC_DANGER), ui_hex(UIC_DANGER));
        smallcaps_center(cv, CX, 272, "CANCEL", ui_hex(UIC_SUB));
        return;
    }

    // topbar: back chevron + small-caps title + count
    ui_chevron(cv, 12, 10, 8, 12, 1, 1.6f, ui_hex(UIC_SUB));
    smallcaps(cv, 26, 7, "CREDENTIALS", ui_hex(UIC_MUTED));
    char cnt[8]; snprintf(cnt, sizeof(cnt), "%d", st->cred_n);
    ui_ftext(cv, F_SMALL, W - 12 - ui_ftext_w(F_SMALL, cnt), 7, cnt, ui_hex(UIC_SUB));
    ui_hline(cv, 12, 32, W - 24, ui_hex(UIC_LINE));

    if (st->cred_n == 0) {
        draw_rings(cv, CX, 120, 24, 8, 18, false, 1);
        ui_ftext_center(cv, F_BODY, CX, 168, "no keys yet", ui_hex(UIC_MUTED));
        ui_ftext_center(cv, F_SMALL, CX, 196, "enroll from the desktop", ui_hex(UIC_MUTED));
    } else {
        int top = st->cred_top;
        int vis = st->cred_n - top; if (vis > CRED_VIS) vis = CRED_VIS; if (vis < 0) vis = 0;
        int y0 = CRED_ROWS_Y(vis);
        for (int i = top; i < st->cred_n && i < top + CRED_VIS; i++) {
            int ry = y0 + (i - top) * (CRED_ROW_H + CRED_ROW_GAP);
            if (i == st->cred_sel) {
                ui_rrect(cv, 8, ry, W - 16, CRED_ROW_H, 12, ui_hex(UIC_SEL));
                ui_rrect_border(cv, 8, ry, W - 16, CRED_ROW_H, 12, 1, ui_hex(UIC_CORE_B));
            }
            // 32px icon tile: favicon from NVS, else a 2-char monogram
            ui_rrect(cv, 16, ry + 14, 32, 32, 8, ui_hex(UIC_CARD));
            if (st->cred_icon[i]) {
                int iw = st->cred_icon_w[i], ih = st->cred_icon_h[i];
                ui_blit(cv, 16 + (32 - iw) / 2, ry + 14 + (32 - ih) / 2, iw, ih, st->cred_icon[i]);
            } else {
                char fi[3] = { (char)toupper_((int)st->cred_rp[i][0]),
                               st->cred_rp[i][0] ? (char)toupper_((int)st->cred_rp[i][1]) : 0, 0 };
                ui_ftext(cv, F_BODY, 16 + (32 - ui_ftext_w(F_BODY, fi)) / 2, ry + 18, fi,
                         ui_hex(UIC_CORE_G));
            }
            char rp[28], us[28];
            ui_ftext_fit(rp, sizeof rp, F_BODY, st->cred_rp[i], 106);
            ui_ftext_fit(us, sizeof us, F_SMALL, st->cred_user[i], 106);
            ui_ftext(cv, F_BODY, 52, ry + 7, rp, ui_hex(UIC_INK));
            ui_ftext(cv, F_SMALL, 52, ry + 33, us, ui_hex(UIC_MUTED));
        }
        // page dots at the right edge when the list overflows
        if (st->cred_n > CRED_VIS) {
            int pages = (st->cred_n + CRED_VIS - 1) / CRED_VIS;
            int cur = top / CRED_VIS;
            int cy = (CRED_LIST_Y0 + CRED_LIST_Y1) / 2 - (pages - 1) * 5;
            for (int pg = 0; pg < pages; pg++) {
                if (pg == cur) ui_disc(cv, 165, cy + pg * 10, 2, ui_hex(UIC_SUB));
                else           ui_disc(cv, 165, cy + pg * 10, 2, ui_hex(UIC_LINE2));
            }
        }
        action_btn_c(cv, CRED_DEL_X, CRED_DEL_Y, CRED_DEL_W, CRED_DEL_H, "DELETE",
                     ui_hex(UIC_DANGER_BG), ui_hex(UIC_DANGER), ui_hex(UIC_DANGER));
    }

    // page dots: credentials is the right page
    ui_ring(cv, 79, 306, 2.5f, 1.2f, ui_hex(UIC_LINE2));
    ui_disc(cv, 91, 306, 2.5f, ui_hex(UIC_SUB));
}

// ---- APPROVAL ----------------------------------------------------------------
static void scr_approve(ui_canvas_t *cv, const ui_state_t *st)
{
    ui_clear(cv, ui_hex(UIC_BG));
    smallcaps(cv, 12, 8, "SIGN IN", ui_hex(UIC_MUTED));
    ui_ftext(cv, F_SMALL, W - 12 - ui_ftext_w(F_SMALL, "FIDO2"), 8, "FIDO2", ui_hex(UIC_CORE_G));
    ui_hline(cv, 12, 30, W - 24, ui_hex(UIC_LINE));

    draw_rings(cv, CX, 74, 26, 8, 20, true, 1);

    // rp id: F_MONO, up to two centered lines of 14 chars (14*11 = 154px);
    // longer ids middle-ellipsize first so the registrable tail stays visible.
    const char *rp_src = st->rp && st->rp[0] ? st->rp : "sign";
    char fit[30];
    ui_ftext_fit_tail(fit, sizeof fit, F_MONO, rp_src, 14 * 11 * 2);
    int len = (int)strlen(fit);
    if (len <= 14) {
        ui_ftext_center(cv, F_MONO, CX, 118, fit, ui_hex(UIC_INK));
    } else {
        char l1[16], l2[16];
        memcpy(l1, fit, 14); l1[14] = 0;
        snprintf(l2, sizeof l2, "%s", fit + 14);
        ui_ftext_center(cv, F_MONO, CX, 118, l1, ui_hex(UIC_INK));
        ui_ftext_center(cv, F_MONO, CX, 144, l2, ui_hex(UIC_INK));
    }

    if (st->forwarded) {
        ui_rrect(cv, 12, 174, W - 24, 26, 8, ui_hex(0x2A1220));
        ui_rrect_border(cv, 12, 174, W - 24, 26, 8, 1, ui_hex(UIC_DANGER));
        ui_ftext_center(cv, F_SMALL, CX, 178, "FORWARDED", ui_hex(UIC_DANGER));
    } else if (st->coauthd) {
        ui_ftext_center(cv, F_SMALL, CX, 178, "desktop co-authorized", ui_hex(UIC_OK));
    }

    action_btn(cv, APPR_BTN_X, APPR_BTN_Y, APPR_BTN_W, APPR_BTN_H, "APPROVE");
    if (st->progress > 0.003f)      // hold-to-approve progress along the inner bottom edge
        ui_rect(cv, APPR_BTN_X + 4, APPR_BTN_Y + APPR_BTN_H - 8,
                (int)((APPR_BTN_W - 8) * clampf(st->progress, 0, 1)), 3, ui_hex(UIC_CORE_G));
    ui_ftext_center(cv, F_SMALL, CX, 302, "tap, or hold BOOT", ui_hex(UIC_MUTED));
}

// ---- PAIRING -----------------------------------------------------------------
static void scr_pair(ui_canvas_t *cv, const ui_state_t *st)
{
    ui_clear(cv, ui_hex(UIC_BG));
    smallcaps(cv, 12, 8, "PAIRING", ui_hex(UIC_MUTED));
    ui_hline(cv, 12, 30, W - 24, ui_hex(UIC_LINE));

    if (!st->sas || !st->sas[0]) {
        // Waiting for the desktop: rings reaching toward each other, core unlit.
        float br = 1.0f + 0.15f * sinf(st->t * 2.2f);
        draw_rings(cv, CX, 108, 34, 11, (int)(26 * br), false, 1);
        ui_ftext_center(cv, F_TITLE, CX, 176, "ENROLL", ui_hex(UIC_CORE_G));
        ui_ftext_center(cv, F_SMALL, CX, 216, "on the desktop run", ui_hex(UIC_MUTED));
        // legacy 8x16 face: the ONE sanctioned call site — a 20-char terminal
        // command that no larger monospace face can fit in 170px.
        ui_text_center(cv, CX, 242, "corekeys-daemon pair", 1, ui_hex(UIC_SUB));
        ui_hline(cv, 30, 274, W - 60, ui_hex(UIC_LINE));
        ui_ftext_center(cv, F_SMALL, CX, 286, "waiting for desktop", ui_hex(UIC_MUTED));
        return;
    }
    draw_rings(cv, CX, 60, 22, 7, 17, true, 1);   // linking
    smallcaps_center(cv, CX, 96, "COMPARE CODE", ui_hex(UIC_MUTED));
    // SAS: two 3-digit groups in the 40px tabular face (6*24 + 14 = 158px)
    int slen = (int)strlen(st->sas);
    if (slen == 6) {
        char g1[4] = { st->sas[0], st->sas[1], st->sas[2], 0 };
        char g2[4] = { st->sas[3], st->sas[4], st->sas[5], 0 };
        int x0 = CX - (6 * 24 + 14) / 2;
        ui_ftext(cv, F_SAS, x0, 122, g1, ui_hex(UIC_INK));
        ui_ftext(cv, F_SAS, x0 + 3 * 24 + 14, 122, g2, ui_hex(UIC_INK));
    } else {
        ui_ftext_center(cv, F_SAS, CX, 122, st->sas, ui_hex(UIC_INK));
    }
    char mfit[24];
    ui_ftext_fit_tail(mfit, sizeof mfit, F_MONO, st->machine ? st->machine : "?", 154);
    ui_ftext_center(cv, F_MONO, CX, 180, mfit, ui_hex(UIC_CORE_G));
    ui_ftext_center(cv, F_SMALL, CX, 208, "unverified", ui_hex(UIC_MUTED));
    action_btn(cv, APPR_BTN_X, APPR_BTN_Y, APPR_BTN_W, APPR_BTN_H, "MATCH");
    ui_ftext_center(cv, F_SMALL, CX, 302, "tap if it matches", ui_hex(UIC_MUTED));
}

// ---- RESULT (brief post-decision) -------------------------------------------
static void scr_result(ui_canvas_t *cv, const ui_state_t *st)
{
    ui_clear(cv, ui_hex(UIC_BG));
    const char *m = st->msg ? st->msg : "";
    // Tint by keyword: signed/paired = mint, denied/error = danger, else core.
    uint16_t c = ui_hex(UIC_CORE_G);
    if (strstr(m, "sign") || strstr(m, "PAIR") || strstr(m, "ok")) c = ui_hex(UIC_OK);
    else if (strstr(m, "den") || strstr(m, "fail") || strstr(m, "err") || strstr(m, "mismatch")) c = ui_hex(UIC_DANGER);
    bool good = (c == ui_hex(UIC_OK));
    draw_rings(cv, CX, 110, 38, 12, 30, good, 1);
    ui_ftext_center(cv, F_BODY, CX, 204, m, c);
}

// ---- QUICK MENU (pull-down curtain over home) --------------------------------
static void menu_row(ui_canvas_t *cv, int y, const char *label, const char *sub,
                     uint16_t label_c, bool pressed)
{
    if (pressed)
        ui_rrect(cv, 12, y + 2, W - 24, MENU_ROW_H - 4, 10, ui_hex(UIC_PRESS));
    ui_ftext(cv, F_TITLE, 20, y + 6, label, label_c);
    ui_ftext(cv, F_SMALL, 20, y + 38, sub, ui_hex(UIC_MUTED));
}

static void scr_menu(ui_canvas_t *cv, const ui_state_t *st)   // overlay; home drawn behind
{
    // Position follows menu_prog linearly so a drag tracks the finger 1:1;
    // easing over time is the tween's job (ui.c), not the renderer's.
    float p = clampf(st->menu_prog, 0, 1);
    int off = (int)((1.0f - p) * CURTAIN_TRAVEL + 0.5f);
    if (off >= H) return;

    // The curtain: a full-screen sheet sliding down over home. No scrim — home
    // stays lit and is simply covered, which is what makes it read as a shade.
    ui_canvas_t v = ui_view(cv, 0, -off, 0, 0, W, H - off);
    ui_clear(&v, ui_hex(UIC_SHEET));            // fills the clipped viewport fast

    status_bar(&v, st->paired, st->batt_pct, st->batt_charging);
    smallcaps(&v, 16, 40, "MENU", ui_hex(UIC_MUTED));
    ui_hline(&v, 16, 64, 138, ui_hex(UIC_LINE));

    int pr = st->menu_pressed;               // 1 + row index, 0 = none
    menu_row(&v, MENU_ROW1_Y, st->has_pin ? "CHANGE PIN" : "SET PIN",
             st->has_pin ? "change unlock PIN" : "protect the device",
             ui_hex(UIC_INK), pr == 1);
    menu_row(&v, MENU_ROW1_Y + MENU_ROW_H, "LOCK NOW",
             st->has_pin ? "re-lock now" : "no PIN set",
             ui_hex(st->has_pin ? UIC_INK : UIC_MUTED), pr == 2);
    menu_row(&v, MENU_ROW1_Y + 2 * MENU_ROW_H, "SLEEP", "screen off",
             ui_hex(UIC_INK), pr == 3);
    menu_row(&v, MENU_ROW1_Y + 3 * MENU_ROW_H, "POWER OFF", "BOOT to wake",
             ui_hex(UIC_DANGER), pr == 4);
    for (int i = 1; i < 4; i++)
        ui_hline(&v, 20, MENU_ROW1_Y + i * MENU_ROW_H - 1, 130, ui_hex(UIC_LINE));

    // bottom edge + handle: the moving hem of the curtain is the close affordance
    ui_rect(&v, 0, H - 2, W, 2, ui_hex(UIC_LINE2));
    ui_rrect(&v, 67, 308, 36, 5, 2, ui_hex(UIC_MUTED));
}

void ui_render(ui_canvas_t *cv, const ui_state_t *st)
{
    if ((st->screen == UI_HOME || st->screen == UI_CREDS) && st->nav_active) {
        // Mid-slide: home and creds are one horizontal strip, offset by nav_x.
        int nx = (int)(st->nav_x + 0.5f);
        if (nx < 0) nx = 0;
        if (nx > W) nx = W;
        if (nx < W) { ui_canvas_t v = ui_view(cv, -nx, 0, 0, 0, W - nx, H); scr_home(&v, st); }
        if (nx > 0) { ui_canvas_t v = ui_view(cv, W - nx, 0, W - nx, 0, W, H); scr_creds(&v, st); }
    } else {
        switch (st->screen) {
        case UI_BOOT:    scr_boot(cv, st); break;
        case UI_LOCK:    scr_lock(cv, st); break;
        case UI_HOME:    scr_home(cv, st); break;
        case UI_CREDS:   scr_creds(cv, st); break;
        case UI_APPROVE: scr_approve(cv, st); break;
        case UI_PAIR:    scr_pair(cv, st); break;
        case UI_RESULT:  scr_result(cv, st); break;
        }
    }
    if (st->screen == UI_HOME && st->menu_prog > 0.003f) scr_menu(cv, st);
}

int ui_hit_test(const ui_state_t *st, int x, int y)
{
    // Curtain owns all taps, but only once fully open — a sheet mid-drag/settle
    // has no valid hitboxes (ui.c also swallows taps during drags).
    if (st->screen == UI_HOME && st->menu_prog >= 0.999f) {
        if (y >= MENU_CLOSE_Y) return UIA_MENU_CLOSE;        // handle band at the hem
        if (y >= MENU_ROW1_Y && y < MENU_ROW1_Y + 4 * MENU_ROW_H) {
            switch ((y - MENU_ROW1_Y) / MENU_ROW_H) {
            case 0: return UIA_SET_PIN;
            case 1: return UIA_MENU_LOCK;
            case 2: return UIA_MENU_SLEEP;
            default: return UIA_MENU_OFF;
            }
        }
        return UIA_NONE;
    }
    switch (st->screen) {
    case UI_LOCK:
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 3; c++) {
                int kx = KP_X0 + c * (KP_W + KP_GX), ky = KP_Y0 + r * (KP_H + KP_GY);
                if (x >= kx && x < kx + KP_W && y >= ky && y < ky + KP_H) {
                    if (r < 3) return UIA_DIGIT + (r * 3 + c + 1);
                    if (c == 0) return UIA_KEY_CLEAR;
                    if (c == 1) return UIA_DIGIT + 0;
                    return UIA_KEY_BACK;
                }
            }
        return UIA_NONE;
    case UI_HOME:
        if (y >= HOME_META_Y0 && y < HOME_META_Y1) return UIA_OPEN_CREDS;
        return UIA_NONE;
    case UI_CREDS:
        if (st->confirm_del) {
            if (y >= CONF_DEL_Y && y < CONF_DEL_Y + CRED_DEL_H &&
                x >= CRED_DEL_X && x < CRED_DEL_X + CRED_DEL_W) return UIA_DEL_DO;
            if (y >= CONF_CANCEL_Y0 && y < CONF_CANCEL_Y1 && x >= 40 && x < 130)
                return UIA_DEL_CANCEL;
            return UIA_NONE;                     // home also cancels
        }
        if (y < 40 && x < 40) return UIA_NAV_HOME;           // back chevron
        if (st->cred_n > 0 && y >= CRED_DEL_Y && y < CRED_DEL_Y + CRED_DEL_H) return UIA_DEL_ASK;
        if (st->cred_n > 0) {
            int vis = st->cred_n - st->cred_top;
            if (vis > CRED_VIS) vis = CRED_VIS;
            int y0 = CRED_ROWS_Y(vis);
            if (y >= y0 && y < y0 + vis * (CRED_ROW_H + CRED_ROW_GAP) - CRED_ROW_GAP) {
                int i = (y - y0) / (CRED_ROW_H + CRED_ROW_GAP);
                if (i >= 0 && i < vis) return UIA_CRED_ROW + i;
            }
        }
        return UIA_NONE;
    case UI_APPROVE:
        if (y >= APPR_BTN_Y && y < APPR_BTN_Y + APPR_BTN_H) return UIA_APPROVE;
        return UIA_NONE;
    case UI_PAIR:
        if (st->sas && st->sas[0] && y >= APPR_BTN_Y && y < APPR_BTN_Y + APPR_BTN_H) return UIA_CONFIRM;
        return UIA_NONE;
    default:
        return UIA_NONE;
    }
}
