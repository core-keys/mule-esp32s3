// Host preview for the device UI: renders every screen (and a couple of
// animation frames) into a contact-sheet BMP, so the look can be iterated
// without flashing the board. Build + run:
//   cc -I main tools/ui_preview.c main/ui_draw.c main/ui_screens.c -lm -o /tmp/uip
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
    ui_state_t frames[10]; int nf = 0;
    #define ADD (frames[nf++])
    ui_state_t s;

    memset(&s, 0, sizeof s); s.screen = UI_BOOT; s.t = 0.35f; ADD = s;      // boot mid
    memset(&s, 0, sizeof s); s.screen = UI_BOOT; s.t = 1.4f;  ADD = s;      // boot final
    memset(&s, 0, sizeof s); s.screen = UI_LOCK; s.pin_max = 4; s.pin_len = 2; s.pin_digit = 7; ADD = s;
    memset(&s, 0, sizeof s); s.screen = UI_LOCK; s.pin_max = 4; s.pin_len = 1; s.pin_digit = 3; s.pin_wrong = true; ADD = s;
    memset(&s, 0, sizeof s); s.screen = UI_HOME; s.paired = true; s.unlocked = true; s.cred_count = 3; s.daemon_count = 1; s.t = 0.6f; ADD = s;

    memset(&s, 0, sizeof s); s.screen = UI_CREDS;
    s.cred_n = 3; s.cred_sel = 0; s.cred_top = 0;
    s.cred_rp[0] = "github.com";    s.cred_user[0] = "felipe";
    s.cred_rp[1] = "example.com";   s.cred_user[1] = "felipe";
    s.cred_rp[2] = "tailscale.com"; s.cred_user[2] = "felipe@core";
    ADD = s;

    memset(&s, 0, sizeof s); s.screen = UI_APPROVE; s.rp = "github.com"; s.coauthd = true; s.progress = 0.42f; ADD = s;
    memset(&s, 0, sizeof s); s.screen = UI_PAIR; s.sas = "418174"; s.machine = "felipe-mbp"; ADD = s;

    // Contact sheet: 4 columns.
    int cols = 4, rows = (nf + cols - 1) / cols;
    int gap = 24, margin = 24;
    int shw = margin * 2 + cols * SW + (cols - 1) * gap;
    int shh = margin * 2 + rows * SH + (rows - 1) * gap;
    uint16_t *sheet = malloc((size_t)shw * shh * 2);
    for (int i = 0; i < shw * shh; i++) sheet[i] = ui_hex(0x04050A);

    uint16_t tile[SW * SH];
    ui_canvas_t cv = { tile, SW, SH };
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
