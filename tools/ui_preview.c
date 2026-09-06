// Host preview for the device UI: renders every screen (and a couple of
// animation frames) into a contact-sheet BMP, so the look can be iterated
// without flashing the board. This sheet is the regression gate before any
// flash. Build + run:
//   cc -I main tools/ui_preview.c main/ui_draw.c main/ui_screens.c main/fonts.c -lm -o /tmp/uip
//   /tmp/uip /tmp/ui_sheet.bmp
#include "ui_screens.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SW 170
#define SH 320

static void wr16(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }
static void wr32(FILE *f, unsigned v) { wr16(f, v & 0xffff); wr16(f, v >> 16); }

// Write a uint16 RGB565 buffer as a 24-bit BMP.
static void write_bmp(const char *path, const uint16_t *px, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror("open"); exit(1); }
    int rowbytes = (w * 3 + 3) & ~3;
    int imgsize = rowbytes * h;
    fputc('B', f); fputc('M', f);
    wr32(f, 54 + imgsize); wr32(f, 0); wr32(f, 54);
    wr32(f, 40); wr32(f, w); wr32(f, h); wr16(f, 1); wr16(f, 24);
    wr32(f, 0); wr32(f, imgsize); wr32(f, 2835); wr32(f, 2835); wr32(f, 0); wr32(f, 0);
    unsigned char *row = calloc(1, rowbytes);
    for (int y = h - 1; y >= 0; y--) {          // BMP rows are bottom-up
        for (int x = 0; x < w; x++) {
            uint16_t c = px[y * w + x];
            int r = ((c >> 11) & 0x1F) << 3, g = ((c >> 5) & 0x3F) << 2, b = (c & 0x1F) << 3;
            row[x * 3 + 0] = b; row[x * 3 + 1] = g; row[x * 3 + 2] = r; // BGR
        }
        fwrite(row, 1, rowbytes, f);
    }
    free(row); fclose(f);
}

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "/tmp/ui_sheet.bmp";

    // Build the frame list.
    ui_state_t frames[28]; int nf = 0;
    #define ADD (frames[nf++])
    ui_state_t s;

    memset(&s, 0, sizeof s); s.screen = UI_BOOT; s.t = 0.35f; ADD = s;      // boot mid
    memset(&s, 0, sizeof s); s.screen = UI_BOOT; s.t = 1.4f;  ADD = s;      // boot final
    memset(&s, 0, sizeof s); s.screen = UI_LOCK; s.pin_max = 4; s.pin_len = 2; s.pin_digit = 7; ADD = s;
    memset(&s, 0, sizeof s); s.screen = UI_LOCK; s.pin_max = 4; s.pin_len = 1; s.pin_digit = 3; s.pin_wrong = true; s.pin_retries = 6; ADD = s;
    memset(&s, 0, sizeof s); s.screen = UI_LOCK; s.pin_max = 4; s.pin_len = 2; s.pin_digit = 5; s.lock_mode = 1; ADD = s;
    memset(&s, 0, sizeof s); s.screen = UI_HOME; s.paired = true; s.unlocked = true; s.has_pin = true; s.cred_count = 3; s.daemon_count = 1; s.batt_pct = -1; s.t = 0.6f; ADD = s;   // battery placeholder (no cell yet)
    memset(&s, 0, sizeof s); s.screen = UI_HOME; s.paired = true; s.unlocked = true; s.has_pin = true; s.cred_count = 3; s.daemon_count = 1; s.batt_pct = 72; s.t = 0.6f; ADD = s;   // battery present
    memset(&s, 0, sizeof s); s.screen = UI_HOME; s.paired = true; s.unlocked = true; s.has_pin = true; s.cred_count = 3; s.daemon_count = 1; s.batt_pct = 63; s.batt_charging = true; s.t = 0.6f; ADD = s;   // charging: bolt over mid fill + %
    memset(&s, 0, sizeof s); s.screen = UI_HOME; s.paired = true; s.unlocked = true; s.has_pin = false; s.cred_count = 0; s.daemon_count = 0; s.batt_pct = 20; s.t = 0.6f; ADD = s;  // fresh device: NO KEYS · NOT PAIRED
    memset(&s, 0, sizeof s); s.screen = UI_HOME; s.paired = true; s.unlocked = true; s.has_pin = true; s.cred_count = 3; s.daemon_count = 1; s.batt_pct = -1; s.menu_open = true; s.menu_prog = 1.0f; s.t = 0.6f; ADD = s;   // curtain fully open
    s.menu_prog = 0.45f; ADD = s;                                        // curtain mid-drag: home visible below the hem
    s.menu_prog = 1.0f; s.menu_pressed = 4; ADD = s;                     // POWER OFF row press feedback

    memset(&s, 0, sizeof s); s.screen = UI_CREDS;
    s.cred_n = 3; s.cred_sel = 0; s.cred_top = 0;
    s.cred_rp[0] = "google.com";       s.cred_user[0] = "felipe@zimmerle.org";
    s.cred_rp[1] = "github.com";        s.cred_user[1] = "felipe";
    s.cred_rp[2] = "login.tailscale.com"; s.cred_user[2] = "felipe@zimmerle.org";
    ADD = s;

    // delete-confirm dialog for the selected (long-email) row
    s.confirm_del = true; s.cred_sel = 0; ADD = s;

    // 8 credentials, page 2 (rows 3..5 visible, page dots): paging regression
    s.confirm_del = false;
    s.cred_n = 8; s.cred_sel = 4; s.cred_top = 3;
    for (int i = 3; i < 8; i++) { s.cred_rp[i] = "site-number-8.example"; s.cred_user[i] = "a-very-long-username@example.org"; }
    ADD = s;

    // home->creds slide, mid-drag: same strip from either committed screen (the
    // two frames must render identically — regression for the composite path)
    memset(&s, 0, sizeof s);
    s.cred_n = 3; s.cred_sel = 0; s.cred_top = 0;
    s.cred_rp[0] = "google.com";       s.cred_user[0] = "felipe@zimmerle.org";
    s.cred_rp[1] = "github.com";        s.cred_user[1] = "felipe";
    s.cred_rp[2] = "login.tailscale.com"; s.cred_user[2] = "felipe@zimmerle.org";
    s.paired = true; s.unlocked = true; s.has_pin = true;
    s.cred_count = 3; s.daemon_count = 1; s.batt_pct = -1; s.t = 0.6f;
    s.nav_active = true;
    s.screen = UI_HOME;  s.nav_x = 50;  ADD = s;
    s.screen = UI_CREDS; s.nav_x = 128; ADD = s;

    // single-credential list (the common case): should sit centered, not top-pinned
    memset(&s, 0, sizeof s); s.screen = UI_CREDS;
    s.cred_n = 1; s.cred_sel = 0; s.cred_top = 0;
    s.cred_rp[0] = "google.com"; s.cred_user[0] = "felipe@zimmerle.org";
    ADD = s;

    // empty list
    memset(&s, 0, sizeof s); s.screen = UI_CREDS; ADD = s;

    memset(&s, 0, sizeof s); s.screen = UI_APPROVE; s.rp = "github.com"; s.coauthd = true; s.progress = 0.42f; ADD = s;
    // hostile rp: middle-ellipsized, registrable tail MUST stay visible
    memset(&s, 0, sizeof s); s.screen = UI_APPROVE; s.rp = "github.com.attacker.example"; s.forwarded = true; ADD = s;
    memset(&s, 0, sizeof s); s.screen = UI_PAIR; s.t = 0.8f; ADD = s;                                   // waiting for desktop
    memset(&s, 0, sizeof s); s.screen = UI_PAIR; s.sas = "418174"; s.machine = "felipe-mbp"; ADD = s;
    memset(&s, 0, sizeof s); s.screen = UI_PAIR; s.sas = "000000"; s.machine = "a-very-long-machine-name.local"; ADD = s;  // tabular-digit + fit_tail check
    memset(&s, 0, sizeof s); s.screen = UI_RESULT; s.msg = "signed prod-db"; ADD = s;
    memset(&s, 0, sizeof s); s.screen = UI_RESULT; s.msg = "denied"; ADD = s;

    // Contact sheet: 4 columns.
    int cols = 4, rows = (nf + cols - 1) / cols;
    int gap = 24, margin = 24;
    int shw = margin * 2 + cols * SW + (cols - 1) * gap;
    int shh = margin * 2 + rows * SH + (rows - 1) * gap;
    uint16_t *sheet = malloc((size_t)shw * shh * 2);
    for (int i = 0; i < shw * shh; i++) sheet[i] = ui_hex(0x04050A);

    uint16_t tile[SW * SH];
    ui_canvas_t cv = { .fb = tile, .w = SW, .h = SH };
    for (int i = 0; i < nf; i++) {
        ui_render(&cv, &frames[i]);
        int c = i % cols, r = i / cols;
        int ox = margin + c * (SW + gap), oy = margin + r * (SH + gap);
        for (int y = 0; y < SH; y++)
            memcpy(&sheet[(oy + y) * shw + ox], &tile[y * SW], SW * 2);
    }
    write_bmp(out, sheet, shw, shh);
    printf("wrote %s (%dx%d, %d frames)\n", out, shw, shh, nf);
    free(sheet);
    return 0;
}
