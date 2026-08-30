// core-keys device screens — drawn with ui_draw, shared by device + host preview.
#pragma once
#include "ui_draw.h"

typedef enum { UI_BOOT, UI_LOCK, UI_HOME, UI_CREDS, UI_APPROVE, UI_PAIR, UI_RESULT } ui_screen_t;

#define UI_MAX_CREDS 8

typedef struct {
    ui_screen_t screen;
    float t;                 // seconds, drives animation

    // lock / pin
    int pin_len, pin_max, pin_digit;
    bool pin_wrong;

    // home
    bool paired, unlocked;
    int cred_count, daemon_count;

    // credentials list
    const char *cred_rp[UI_MAX_CREDS];
    const char *cred_user[UI_MAX_CREDS];
    int cred_n, cred_sel, cred_top;

    // approval
    const char *rp;
    bool coauthd, forwarded;
    float progress;          // 0..1 hold-to-approve

    // pairing
    const char *sas, *machine;

    // result / transient message
    const char *msg;
} ui_state_t;

// Render the active screen into the canvas.
void ui_render(ui_canvas_t *cv, const ui_state_t *st);

// Palette (0xRRGGBB); exposed so device code can theme incidental bits too.
#define UIC_BG      0x080B18
#define UIC_CARD    0x131A33
#define UIC_LINE    0x212B4E
#define UIC_LINE2   0x2C386A
#define UIC_RING_A  0x9BB0EE
#define UIC_RING_P  0x6D5EF0
#define UIC_CORE    0x483BB1
#define UIC_CORE_B  0x6D5EF0
#define UIC_CORE_G  0x8B7DFF
#define UIC_SPARK   0xE9E6FF
#define UIC_INK     0xEAEDFB
#define UIC_SUB     0xAEB6D6
#define UIC_MUTED   0x7A85AC
#define UIC_OK      0x4ED8A6
#define UIC_DANGER  0xFF5D6C
