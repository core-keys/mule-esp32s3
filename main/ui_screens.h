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
    int lock_mode;           // 0 = unlock, 1 = set new PIN, 2 = confirm new PIN
    bool pin_wrong;
    int pin_retries;         // shown on the unlock screen after a wrong entry

    // home
    bool paired, unlocked, has_pin;
    int cred_count, daemon_count;
    int batt_pct;            // 0..100 charge, or -1 = no battery / unknown (placeholder)
    bool batt_charging;      // USB attached: show a charging bolt, not a (bogus) %
    bool menu_open;          // quick-menu curtain logical state (animation target)
    float menu_prog;         // curtain position, 0 = retracted .. 1 = fully open
    int menu_pressed;        // 1 + row index under a finger while fully open, 0 = none

    // home<->creds horizontal slide (drag or tween); `screen` holds the target
    bool nav_active;         // mid-transition: composite home + creds side by side
    float nav_x;             // 0 = home centered .. 170 = creds centered (px of travel)

    // credentials list
    const char *cred_rp[UI_MAX_CREDS];
    const char *cred_user[UI_MAX_CREDS];
    const uint16_t *cred_icon[UI_MAX_CREDS];   // NULL = no icon, draw the monogram instead
    uint8_t cred_icon_w[UI_MAX_CREDS], cred_icon_h[UI_MAX_CREDS];
    int cred_n, cred_sel, cred_top;
    bool confirm_del;        // credentials screen: showing the delete-confirm dialog

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

// Touch hit-testing. Given a tap in DISPLAY coords, return the action for the
// current screen. Digit keys return UIA_DIGIT + d (0..9); credential rows
// return UIA_CRED_ROW + visible-row-index. The drawn targets and these
// hitboxes share the geometry macros below so they can't drift.
enum {
    UIA_NONE = 0,
    UIA_KEY_CLEAR, UIA_KEY_BACK,     // keypad: clear all / backspace
    UIA_OPEN_CREDS, UIA_SET_PIN,     // home
    UIA_APPROVE, UIA_CONFIRM,        // approve / pairing confirm
    UIA_NAV_HOME,                    // credentials: back chevron -> home
    UIA_DEL_ASK, UIA_DEL_DO, UIA_DEL_CANCEL,   // credentials: delete flow
    UIA_MENU_CLOSE,                  // quick-menu curtain: dismiss
    UIA_MENU_SLEEP, UIA_MENU_LOCK, UIA_MENU_OFF,   // quick-menu actions
    UIA_DIGIT = 100,                 // + d (0..9)
    UIA_CRED_ROW = 200               // + visible row index (0..CRED_VIS-1)
};
int ui_hit_test(const ui_state_t *st, int x, int y);

// The capacitive home button sits below the display (y >= this in touch coords).
#define UI_HOME_BTN_Y 325

// Quick-menu curtain: a full-screen sheet sliding down from the top edge.
#define CURTAIN_TRAVEL 320           // px the sheet slides to fully hide above
#define MENU_ROW_H 58
#define MENU_ROW1_Y 72               // row tops: 72 / 130 / 188 / 246 (sheet-local)
#define MENU_CLOSE_Y 304             // taps at/below this close the curtain

// Credentials list: shared by renderer, hit-test, and ui.c paging.
#define CRED_VIS 3
#define CRED_ROW_H 60
#define CRED_ROW_GAP 4
#define CRED_LIST_Y0 36
#define CRED_LIST_Y1 232
#define CRED_ROWS_Y(vis) (CRED_LIST_Y0 + ((CRED_LIST_Y1 - CRED_LIST_Y0) \
        - ((vis) * CRED_ROW_H + ((vis) - 1) * CRED_ROW_GAP)) / 2)

// Palette (0xRRGGBB); exposed so device code can theme incidental bits too.
#define UIC_BG      0x080B18
#define UIC_SHEET   0x0C1122
#define UIC_CARD    0x131A33
#define UIC_PRESS   0x1A2350
#define UIC_SEL     0x1B2450
#define UIC_LINE    0x212B4E
#define UIC_LINE2   0x2C386A
#define UIC_BTN     0x2A2F6E
#define UIC_DANGER_BG 0x2A1622
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
#define UIC_WARN    0xE9B44A
#define UIC_DANGER  0xFF5D6C
