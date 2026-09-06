// core-keys device UI task. See ui.h.
#include "ui.h"
#include "ui_screens.h"
#include "lcd.h"
#include "ck_store.h"
#include "ck_touch.h"
#include "ck_batt.h"
#include "session.h"                 // ck_button_init
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

#define BTN_ADV  GPIO_NUM_14         // advance: +1 digit / scroll
#define BTN_SEL  GPIO_NUM_0          // BOOT: commit / open / back
#define LONG_MS  700
#define RESULT_TIMEOUT_S 4.0f        // auto-dismiss the transient RESULT screen
#define BATT_DEMO 0                  // 1 = sweep the gauge for a preview; 0 = read the real cell via ck_batt
#define SLEEP_IDLE_MS    60000u      // screen off after 1 min idle
#define POWEROFF_IDLE_MS 300000u     // deep sleep after 5 min idle (battery only)
#define UI_CURTAIN_PEEK 1            // once per boot: peek the curtain 12px to hint the pull

// Shared UI state, guarded by g_mutex. String fields live in these buffers so a
// setter's pointer never dangles; the task deep-copies them into a private
// snapshot under the lock, then renders without holding it.
static ui_state_t g_ui;
static char g_rp[48], g_sas[12], g_machine[40], g_msg[40];
static SemaphoreHandle_t g_mutex;
static uint16_t *g_fb;
static int g_w, g_h;

static bool g_unlocked = true;       // no PIN set => unlocked
static volatile bool g_screen_off;   // SLEEP: backlight off, wake on input / activity
static volatile bool g_power_off;    // POWER OFF: request deep sleep
static volatile uint32_t g_last_activity;  // ms of the last input / CTAP event (idle timer)
static char g_pin[8], g_newpin[8];   // PIN being entered / the first set-pass
static char g_cred_rp[UI_MAX_CREDS][40], g_cred_user[UI_MAX_CREDS][40];

// Set when the on-screen APPROVE / MATCH button is tapped; consumed once by the
// waiting worker (co-auth / SSH / pairing) as an approval, same as a BOOT press.
static volatile bool g_touch_approve;
bool ui_touch_approve_taken(void)
{
    bool t = g_touch_approve; g_touch_approve = false; return t;
}

#define LOCK()   xSemaphoreTake(g_mutex, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(g_mutex)

bool ui_is_unlocked(void) { return g_unlocked; }

// ---- gestures + animation ----------------------------------------------------
// Swipes are recognized in software from the polled touch point stream (the
// CST328 emits no controller gesture codes at all), and the menu card / the
// home<->creds strip track the finger 1:1 during a drag. On release the
// position settles with a short eased tween. All of this state belongs to the
// UI task; setters touch it only through reset_transients() under the lock.
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static float lerp(float a, float b, float t) { return a + (b - a) * t; }
static float ease_out(float p) { p = clampf(p, 0, 1); return 1 - (1 - p) * (1 - p); }

typedef struct { bool active; float from, to; uint32_t t0, dur; } tween_t;
static tween_t s_tw_menu, s_tw_nav;      // settle animations for menu_prog / nav_x

typedef enum { G_IDLE, G_TOUCH, G_DRAG_MENU, G_DRAG_NAV, G_DRAG_LIST, G_DEAD } gstate_t;
static gstate_t s_gs;
static int s_gx0, s_gy0;                 // press point
static int s_glx, s_gly;                 // last point (velocity, frozen on a missed sample)
static uint32_t s_glms;                  // ms of the last point
static float s_gvx, s_gvy;               // EMA finger velocity, px/s
static float s_gbase;                    // menu travel px / nav_x at drag start
static ui_screen_t s_gscreen;            // screen the gesture started on (abort guard)
static int s_release_miss;               // consecutive missed samples (I2C-glitch guard)

#define AXIS_LOCK 10                     // px of travel before a touch classifies as a drag
#define COMMIT_FRAC 0.3333f              // travel fraction that commits on release
#define FLICK_PXPS 200.0f                // flick velocity that commits regardless of travel
#define MENU_SETTLE_MS 220               // equal perceived velocity over the 320px travel
#define NAV_SETTLE_MS 200
#define NAV_TAP_MS 240                   // full-travel slide for tap/button-initiated moves
#define RELEASE_MISS_MAX 1               // missed samples tolerated before a release is real
#define LIST_PAGE_PX 30                  // vertical drag that pages the credential list

// One-shot curtain peek state (suppressed once the curtain has opened this boot).
static bool s_peeked;
static int s_peek_phase;                 // 0 = idle, 1 = out, 2 = returning
static uint32_t s_home_since;            // ms when HOME was last entered

// The curtain row a finger is resting on (0..3, -1 = none); mirrored into
// ui_state_t.menu_pressed as 1 + row (0 = none) for the press-feedback fill.
static int menu_row_at(int y)
{
    if (y >= MENU_ROW1_Y && y < MENU_ROW1_Y + 4 * MENU_ROW_H)
        return (y - MENU_ROW1_Y) / MENU_ROW_H;
    return -1;
}

// Kill every in-flight gesture/animation; a setter that hard-switches screens
// (approval, result, lock) calls this so the next frame renders a clean cut.
static void reset_transients(void)   // caller holds the lock
{
    g_ui.menu_open = false; g_ui.menu_prog = 0;
    g_ui.menu_pressed = 0;
    g_ui.nav_active = false; g_ui.nav_x = 0;
    s_tw_menu.active = false; s_tw_nav.active = false;
    s_peek_phase = 0;
    if (s_gs == G_DRAG_MENU || s_gs == G_DRAG_NAV || s_gs == G_DRAG_LIST || s_gs == G_TOUCH)
        s_gs = G_DEAD;
}

// Reset the idle timer and wake the display. Called on any input (ui_task) and on
// any CTAP / session event (the setters below), so a sign-in wakes a slept screen.
static void note_activity(void)
{
    g_last_activity = xTaskGetTickCount() * portTICK_PERIOD_MS;
    g_screen_off = false;
}

void ui_init(uint16_t *fb, int w, int h)
{
    g_fb = fb; g_w = w; g_h = h;
    g_mutex = xSemaphoreCreateMutex();
    memset(&g_ui, 0, sizeof g_ui);
    g_ui.screen = UI_BOOT;
    g_ui.pin_max = 4;
    g_ui.batt_pct = -1;                  // no battery cell yet -> status bar shows a placeholder
    g_rp[0] = g_sas[0] = g_machine[0] = g_msg[0] = 0;
    g_ui.rp = g_rp; g_ui.sas = g_sas; g_ui.machine = g_machine; g_ui.msg = g_msg;
}

void ui_lock_boot(void)
{
    g_unlocked = !ck_store_has_pin();
}

// ---- state setters (any task) -----------------------------------------------
void ui_boot_splash(void)
{
    LOCK(); reset_transients(); g_ui.screen = UI_BOOT; g_ui.t = 0; UNLOCK();
}

// Refresh the home-screen fields without goto_rest's transient reset, so an
// animated slide back home can update them mid-flight.
static void home_refresh(void)   // caller holds the lock
{
    int d = ck_store_auth_count();
    g_ui.paired = d > 0; g_ui.daemon_count = d;
    g_ui.cred_count = ck_store_cred_count();
    g_ui.unlocked = true;
}

// Return to the resting screen: lock screen if a PIN gates the device, else home.
static void goto_rest(void)   // caller holds the lock
{
    g_ui.has_pin = ck_store_has_pin();
    reset_transients();
    if (!g_unlocked) {
        g_ui.screen = UI_LOCK; g_ui.lock_mode = 0;
        g_ui.pin_len = 0; g_ui.pin_digit = 0; g_ui.pin_wrong = false;
        return;
    }
    home_refresh();
    g_ui.screen = UI_HOME;
}

// Enter screen-off SLEEP. Sleeping locks the session when a PIN gates the
// device: wake lands on the lock keypad, and ops that arrive while asleep are
// refused as "device locked" — the same posture as a fresh boot. (Deep-sleep
// POWER OFF gets this for free: wake is a reboot through ui_lock_boot.)
static void enter_sleep(void)   // caller holds the lock
{
    if (ck_store_has_pin()) g_unlocked = false;
    goto_rest();
    g_screen_off = true;
}

void ui_note_ctap(const char *cmd) { (void)cmd; note_activity(); LOCK(); goto_rest(); UNLOCK(); }

void ui_approval(const char *who, bool forwarded)
{
    note_activity();
    LOCK();
    snprintf(g_rp, sizeof g_rp, "%s", who ? who : "");
    g_ui.forwarded = forwarded; g_ui.coauthd = false; g_ui.progress = 0;
    g_ui.screen = UI_APPROVE;
    g_touch_approve = false;                  // start fresh; no stale tap
    UNLOCK();
}

void ui_result(const char *msg)
{
    note_activity();
    LOCK();
    snprintf(g_msg, sizeof g_msg, "%s", msg ? msg : "");
    g_ui.screen = UI_RESULT; g_ui.t = 0;
    UNLOCK();
}

void ui_enroll(void)
{
    note_activity();
    LOCK();
    g_sas[0] = 0; g_machine[0] = 0;
    g_ui.screen = UI_PAIR; g_ui.t = 0;
    UNLOCK();
}

void ui_pair_sas(uint32_t sas, const uint8_t *machine, uint16_t machine_len)
{
    note_activity();
    LOCK();
    snprintf(g_sas, sizeof g_sas, "%06lu", (unsigned long)(sas % 1000000UL));
    uint16_t n = machine_len < sizeof(g_machine) - 1 ? machine_len : sizeof(g_machine) - 1;
    memcpy(g_machine, machine, n); g_machine[n] = 0;
    g_ui.screen = UI_PAIR;
    g_touch_approve = false;                  // start fresh; no stale tap
    UNLOCK();
}

// ---- buttons ----------------------------------------------------------------
static bool s_prev_adv, s_prev_sel;
static uint32_t s_down_adv, s_down_sel;
static bool s_long_adv, s_long_sel;

static uint32_t now_ms(void) { return xTaskGetTickCount() * portTICK_PERIOD_MS; }

// Edge decode: *adv/*sel = short press (on release), *ladv = long-press (fires once).
static void poll_buttons(bool *adv, bool *sel, bool *ladv)
{
    bool a = gpio_get_level(BTN_ADV) == 0;
    bool s = gpio_get_level(BTN_SEL) == 0;
    uint32_t t = now_ms();
    *adv = *sel = *ladv = false;

    if (a && !s_prev_adv) { s_down_adv = t; s_long_adv = false; }
    if (a && !s_long_adv && t - s_down_adv >= LONG_MS) { *ladv = true; s_long_adv = true; }
    if (!a && s_prev_adv && !s_long_adv && t - s_down_adv < LONG_MS) *adv = true;

    if (s && !s_prev_sel) { s_down_sel = t; s_long_sel = false; }
    if (!s && s_prev_sel && !s_long_sel && t - s_down_sel < LONG_MS) *sel = true;

    s_prev_adv = a; s_prev_sel = s;
}

static uint16_t s_cred_icon[UI_MAX_CREDS][24 * 24];

// Fill the credential list fields (without switching screens, so a drag can
// populate the incoming pane before the transition commits).
static void creds_populate(void)   // caller holds the lock
{
    int n = ck_store_cred_count();
    if (n > UI_MAX_CREDS) n = UI_MAX_CREDS;
    for (int i = 0; i < UI_MAX_CREDS; i++) g_ui.cred_icon[i] = NULL;  // well-defined
    for (int i = 0; i < n; i++) {
        ck_store_cred_get(i, g_cred_rp[i], 40, g_cred_user[i], 40);
        g_ui.cred_rp[i] = g_cred_rp[i];
        g_ui.cred_user[i] = g_cred_user[i];
        uint8_t w = 0, h = 0;
        if (ck_store_icon_get(g_cred_rp[i], (uint8_t *)s_cred_icon[i],
                              sizeof(s_cred_icon[i]), &w, &h) == 0) {
            g_ui.cred_icon[i] = s_cred_icon[i];
            g_ui.cred_icon_w[i] = w; g_ui.cred_icon_h[i] = h;
        }
    }
    g_ui.cred_n = n; g_ui.cred_sel = 0; g_ui.cred_top = 0;
    g_ui.confirm_del = false;
}

static void load_creds(void)   // caller holds the lock; instant (in-place reload)
{
    creds_populate();
    g_ui.screen = UI_CREDS;
}

// ---- animated transitions ----------------------------------------------------
static void tween_start(tween_t *tw, float from, float to, uint32_t dur_ms)
{
    tw->active = true; tw->from = from; tw->to = to;
    tw->t0 = now_ms(); tw->dur = dur_ms;
}
static float tween_val(tween_t *tw, uint32_t now)   // clears .active on landing
{
    float p = tw->dur ? (float)(now - tw->t0) / (float)tw->dur : 1.0f;
    if (p >= 1.0f) { tw->active = false; return tw->to; }
    return lerp(tw->from, tw->to, ease_out(p));
}

static void menu_anim(bool open)   // caller holds the lock
{
    g_ui.menu_open = open;
    if (open) s_peeked = true;               // real open: the peek hint is moot
    s_peek_phase = 0;
    tween_start(&s_tw_menu, g_ui.menu_prog, open ? 1.0f : 0.0f, MENU_SETTLE_MS);
}
static void menu_snap_closed(void)   // caller holds the lock
{
    g_ui.menu_open = false; g_ui.menu_prog = 0; s_tw_menu.active = false;
}

// Slide the home<->creds strip. `screen` switches to the target immediately (it
// is the committed state, and what hit-tests and setters see); nav_x animates
// to catch up, and the tween-landing check in ui_task clears nav_active.
static void nav_to_creds_anim(uint32_t dur_ms)   // caller holds the lock
{
    if (!g_ui.nav_active) { creds_populate(); g_ui.nav_x = 0; g_ui.nav_active = true; }
    g_ui.screen = UI_CREDS;
    tween_start(&s_tw_nav, g_ui.nav_x, (float)g_w, dur_ms);
}
static void nav_to_home_anim(uint32_t dur_ms)   // caller holds the lock
{
    if (!g_unlocked) { goto_rest(); return; }    // PIN gate: no animated shortcut
    g_ui.has_pin = ck_store_has_pin();
    home_refresh();
    if (!g_ui.nav_active) { g_ui.nav_x = (float)g_w; g_ui.nav_active = true; }
    g_ui.screen = UI_HOME;
    tween_start(&s_tw_nav, g_ui.nav_x, 0.0f, dur_ms);
}

static void pin_complete(void)   // caller holds the lock; g_pin is NUL-terminated
{
    if (g_ui.lock_mode == 0) {                   // unlock
        int r = ck_store_pin_verify(g_pin);
        if (r == 0) { g_unlocked = true; goto_rest(); }
        else { g_ui.pin_wrong = true; g_ui.pin_len = 0; g_ui.pin_digit = 0;
               g_ui.pin_retries = ck_store_pin_retries_left(); }
    } else if (g_ui.lock_mode == 1) {            // set: remember, ask to confirm
        strcpy(g_newpin, g_pin);
        g_ui.lock_mode = 2; g_ui.pin_len = 0; g_ui.pin_digit = 0; g_ui.pin_wrong = false;
    } else {                                     // confirm
        if (strcmp(g_newpin, g_pin) == 0) {
            ck_store_pin_set(g_newpin);
            g_unlocked = true;
            snprintf(g_msg, sizeof g_msg, "PIN set"); g_ui.screen = UI_RESULT; g_ui.t = 0;
        } else {
            g_ui.lock_mode = 1; g_ui.pin_len = 0; g_ui.pin_digit = 0; g_ui.pin_wrong = true;
        }
    }
}

static void handle_buttons(bool adv, bool sel, bool ladv)   // caller holds the lock
{
    switch (g_ui.screen) {
    case UI_LOCK:
        if (adv) { g_ui.pin_digit = (g_ui.pin_digit + 1) % 10; g_ui.pin_wrong = false; }
        if (ladv) { g_ui.pin_len = 0; g_ui.pin_digit = 0; g_ui.pin_wrong = false; }  // restart
        if (sel) {
            if (g_ui.pin_len < g_ui.pin_max) {
                g_pin[g_ui.pin_len++] = (char)('0' + g_ui.pin_digit);
                g_ui.pin_digit = 0;
                if (g_ui.pin_len >= g_ui.pin_max) { g_pin[g_ui.pin_len] = 0; pin_complete(); }
            }
        }
        break;
    case UI_HOME:
        if (sel) nav_to_creds_anim(NAV_TAP_MS);   // BOOT -> credentials (slide in)
        if (ladv) { g_ui.screen = UI_LOCK; g_ui.lock_mode = 1;  // long-advance -> set PIN
                    g_ui.pin_len = 0; g_ui.pin_digit = 0; g_ui.pin_wrong = false; }
        break;
    case UI_CREDS:
        if (g_ui.confirm_del) {                   // in the confirm dialog
            if (adv) { if (g_ui.cred_sel >= 0 && g_ui.cred_sel < g_ui.cred_n)
                           ck_store_cred_del(g_ui.cred_sel);
                       load_creds(); }             // IO14 confirms
            if (sel || ladv) g_ui.confirm_del = false;   // BOOT cancels
            break;
        }
        if (adv && g_ui.cred_n > 0) {
            g_ui.cred_sel = (g_ui.cred_sel + 1) % g_ui.cred_n;
            if (g_ui.cred_sel < g_ui.cred_top) g_ui.cred_top = g_ui.cred_sel;
            if (g_ui.cred_sel >= g_ui.cred_top + CRED_VIS)
                g_ui.cred_top = g_ui.cred_sel - (CRED_VIS - 1);
        }
        if (ladv && g_ui.cred_n > 0) g_ui.confirm_del = true;   // long-IO14 = delete
        if (sel) nav_to_home_anim(NAV_TAP_MS);    // BOOT = back home (slide out)
        break;
    default:
        break;                                    // APPROVE/PAIR/RESULT own their buttons
    }
}

// ---- touch input ------------------------------------------------------------
// Append one digit to the PIN (keypad tap), completing when full.
static void pin_key_digit(int d)   // caller holds the lock
{
    g_ui.pin_wrong = false;
    if (g_ui.pin_len < g_ui.pin_max) {
        g_pin[g_ui.pin_len++] = (char)('0' + d);
        if (g_ui.pin_len >= g_ui.pin_max) { g_pin[g_ui.pin_len] = 0; pin_complete(); }
    }
}

static void handle_action(int a)   // caller holds the lock
{
    if (a >= UIA_CRED_ROW) {                  // tap selects the tapped row
        int i = a - UIA_CRED_ROW;
        if (g_ui.cred_top + i < g_ui.cred_n) g_ui.cred_sel = g_ui.cred_top + i;
        return;
    }
    if (a >= UIA_DIGIT) { pin_key_digit(a - UIA_DIGIT); return; }
    switch (a) {
    case UIA_KEY_CLEAR:
        g_ui.pin_len = 0; g_ui.pin_digit = 0; g_ui.pin_wrong = false;
        break;
    case UIA_KEY_BACK:
        if (g_ui.pin_len > 0) g_ui.pin_len--;
        g_ui.pin_wrong = false;
        break;
    case UIA_OPEN_CREDS:
        nav_to_creds_anim(NAV_TAP_MS);
        break;
    case UIA_SET_PIN:
        g_ui.screen = UI_LOCK; g_ui.lock_mode = 1;
        g_ui.pin_len = 0; g_ui.pin_digit = 0; g_ui.pin_wrong = false;
        break;
    case UIA_NAV_HOME:
        nav_to_home_anim(NAV_TAP_MS);             // creds back chevron
        break;
    case UIA_DEL_ASK:
        if (g_ui.cred_n > 0) g_ui.confirm_del = true;
        break;
    case UIA_DEL_DO:
        if (g_ui.cred_sel >= 0 && g_ui.cred_sel < g_ui.cred_n)
            ck_store_cred_del(g_ui.cred_sel);
        load_creds();                             // reload + clears confirm_del
        break;
    case UIA_DEL_CANCEL:
        g_ui.confirm_del = false;
        break;
    case UIA_APPROVE:
    case UIA_CONFIRM:
        g_touch_approve = true;                   // consumed by the waiting worker
        break;
    case UIA_MENU_CLOSE:
        menu_anim(false);                         // animated retract
        break;
    case UIA_MENU_SLEEP:
        menu_snap_closed();                       // screen is going dark: no animation
        enter_sleep();                            // ui_task turns the backlight off
        break;
    case UIA_MENU_LOCK:
        if (ck_store_has_pin()) { menu_snap_closed(); g_unlocked = false; goto_rest(); }
        else menu_anim(false);                    // "no PIN set": just retract
        break;
    case UIA_MENU_OFF:
        menu_snap_closed();
        g_power_off = true;                       // ui_task enters deep sleep
        break;
    default:
        break;
    }
}

// The capacitive button below the display: back / dismiss on most screens.
static void handle_home(void)   // caller holds the lock
{
    if (g_ui.screen == UI_HOME && (g_ui.menu_open || g_ui.menu_prog > 0.003f)) {
        menu_anim(false);                         // retract the quick menu
        return;
    }
    switch (g_ui.screen) {
    case UI_LOCK:
        if (g_ui.lock_mode == 0) {                // unlocking: clear the entry
            g_ui.pin_len = 0; g_ui.pin_digit = 0; g_ui.pin_wrong = false;
        } else {
            goto_rest();                          // cancel set-PIN -> home
        }
        break;
    case UI_CREDS:
        if (g_ui.confirm_del) { g_ui.confirm_del = false; break; }   // cancel delete
        nav_to_home_anim(NAV_TAP_MS);
        break;
    case UI_RESULT:
        goto_rest();
        break;
    default:
        break;                                    // HOME/APPROVE/PAIR: home does nothing
    }
}

// ---- the render task --------------------------------------------------------
static char s_rp[48], s_sas[12], s_machine[40], s_msg[40];

static uint8_t s_touch_addr;

static bool s_prev_touch;                // touch edge state
static uint32_t s_prev_frame_ms;         // wall clock of the previous frame (drives dt)
static bool s_bl_on = true;              // backlight state (starts on after lcd_init)

static void ui_task(void *arg)
{
    (void)arg;
    ck_button_init();
    s_touch_addr = ck_touch_init();
    g_last_activity = now_ms();           // start the idle timer at boot
    s_prev_frame_ms = g_last_activity;
    ui_canvas_t cv = { .fb = g_fb, .w = g_w, .h = g_h };
    ui_state_t snap;
    for (;;) {
        bool adv, sel, ladv;
        poll_buttons(&adv, &sel, &ladv);

        // Touch. Controller gesture codes are ignored (the CST328 has none);
        // swipes are recognized from the point stream below. y >= UI_HOME_BTN_Y
        // is the capacitive button below the panel.
        int tx = 0, ty = 0; uint8_t gest = 0;
        bool touched = s_touch_addr && ck_touch_read(&tx, &ty, &gest);
        (void)gest;

        uint32_t now = now_ms();
        if (touched || adv || sel || ladv ||
            !gpio_get_level(BTN_SEL) || !gpio_get_level(BTN_ADV))
            g_last_activity = now;                  // any input resets the idle timer

        // POWER OFF: deep sleep, wake on BOOT (GPIO0 low). No return. On battery the
        // system rail is gated by PWR_EN (GPIO15, see lcd.c) — hold it (and the BOOT
        // wake pin) high through sleep, else the rail droops and the chip reboots.
        if (g_power_off) {
            lcd_backlight(false);
            gpio_hold_en(GPIO_NUM_15);
            gpio_deep_sleep_hold_en();
            rtc_gpio_pullup_en(BTN_SEL);
            rtc_gpio_pulldown_dis(BTN_SEL);
            esp_sleep_enable_ext0_wakeup(BTN_SEL, 0);
            esp_deep_sleep_start();
        }
        // Backlight tracks the sleep flag; a CTAP event can clear g_screen_off to wake us.
        if (!g_screen_off && !s_bl_on) { lcd_backlight(true); s_bl_on = true; }

        // SLEEP: backlight off; wake on a fresh touch/button once inputs release
        // (so the tap that slept it doesn't wake it), and escalate to POWER OFF
        // after the longer idle window when running on battery.
        if (g_screen_off) {
            if (s_bl_on) { lcd_backlight(false); s_bl_on = false; }
            // No auto deep-sleep: the S3 can't tell battery from USB (VBUS forced
            // valid), and powering off at the desktop would drop the key / SSH
            // agent. Deep sleep stays a deliberate POWER OFF from the quick menu.
            static bool armed;
            bool up = !touched && gpio_get_level(BTN_SEL) && gpio_get_level(BTN_ADV);
            bool down = touched || !gpio_get_level(BTN_SEL) || !gpio_get_level(BTN_ADV);
            if (up) armed = true;
            if (armed && down) {
                armed = false; g_screen_off = false;
                g_last_activity = now;
                s_gs = G_DEAD; s_release_miss = 0;   // the waking touch starts no drag
                // Swallow the waking input: a held tap must not type a PIN
                // digit, and the wake button's release must not commit one.
                s_prev_touch = true; s_long_adv = s_long_sel = true;
                vTaskDelay(pdMS_TO_TICKS(200));
            } else {
                vTaskDelay(pdMS_TO_TICKS(60));
            }
            continue;
        }

        // A missed sample inside a gesture (finger skid, I2C glitch — read errors
        // also return "not touched") must not read as a release: hold the last
        // point for up to RELEASE_MISS_MAX frames before the release is real.
        bool eff = touched;
        if (touched) s_release_miss = 0;
        else if ((s_gs == G_TOUCH || s_gs == G_DRAG_MENU || s_gs == G_DRAG_NAV)
                 && s_release_miss < RELEASE_MISS_MAX) {
            s_release_miss++; eff = true; tx = s_glx; ty = s_gly;
        }
        bool press = eff && !s_prev_touch;
        bool release = !eff && s_prev_touch;
        s_prev_touch = eff;

        LOCK();
        if (adv || sel || ladv) handle_buttons(adv, sel, ladv);

        // A button or setter switched screens under an active gesture: go inert.
        if (s_gs != G_IDLE && s_gs != G_DEAD && g_ui.screen != s_gscreen) s_gs = G_DEAD;

        if (press) {
            if (ty >= UI_HOME_BTN_Y) { handle_home(); s_gs = G_DEAD; }
            else if (g_ui.screen == UI_HOME || g_ui.screen == UI_CREDS) {
                // Defer the tap to the release so it can become a drag; catch a
                // settling card/pane by freezing its tween where it is.
                s_gs = G_TOUCH; s_gscreen = g_ui.screen;
                s_gx0 = s_glx = tx; s_gy0 = s_gly = ty; s_glms = now;
                s_gvx = s_gvy = 0;
                s_tw_menu.active = false; s_tw_nav.active = false;
                s_peek_phase = 0;
                if (g_ui.screen == UI_HOME && g_ui.menu_prog >= 0.999f)
                    g_ui.menu_pressed = 1 + menu_row_at(ty);   // curtain press feedback
            } else {
                // Keypad / approve / pair stay on the press edge (instant).
                int act = ui_hit_test(&g_ui, tx, ty);
                if (act) handle_action(act);
                s_gs = G_DEAD;
            }
        } else if (eff && s_gs != G_IDLE && s_gs != G_DEAD) {
            uint32_t fdt = now - s_glms;
            if (fdt > 0) {
                float inv = 1000.0f / (float)fdt;
                s_gvx = 0.6f * s_gvx + 0.4f * (float)(tx - s_glx) * inv;
                s_gvy = 0.6f * s_gvy + 0.4f * (float)(ty - s_gly) * inv;
                s_glx = tx; s_gly = ty; s_glms = now;
            }
            if (s_gs == G_TOUCH) {
                int dx = tx - s_gx0, dy = ty - s_gy0;
                int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
                if (adx >= AXIS_LOCK || ady >= AXIS_LOCK) {   // classify: dominant axis
                    g_ui.menu_pressed = 0;                    // a drag is not a row press
                    if (ady >= adx && g_ui.screen == UI_HOME && !g_ui.nav_active &&
                        ((dy > 0 && !g_ui.menu_open) || (dy < 0 && g_ui.menu_prog > 0.003f))) {
                        s_gs = G_DRAG_MENU;                   // pull the curtain down / up
                        s_gbase = g_ui.menu_prog * (float)CURTAIN_TRAVEL;
                    } else if (adx > ady && g_ui.screen == UI_HOME &&
                               g_ui.menu_prog <= 0.003f && dx < 0) {
                        if (!g_ui.nav_active) {               // swipe left: creds slide in
                            creds_populate(); g_ui.nav_x = 0; g_ui.nav_active = true;
                        }
                        s_gs = G_DRAG_NAV; s_gbase = g_ui.nav_x;
                    } else if (adx > ady && g_ui.screen == UI_CREDS &&
                               !g_ui.confirm_del && dx > 0) {
                        if (!g_ui.nav_active) {               // swipe right: back home
                            g_ui.nav_x = (float)g_w; g_ui.nav_active = true;
                        }
                        s_gs = G_DRAG_NAV; s_gbase = g_ui.nav_x;
                    } else if (ady >= adx && g_ui.screen == UI_CREDS &&
                               !g_ui.confirm_del && g_ui.cred_n > CRED_VIS) {
                        s_gs = G_DRAG_LIST;                   // vertical flick pages the list
                    } else {
                        s_gs = G_DEAD;
                    }
                }
            }
            // Track the finger 1:1.
            if (s_gs == G_DRAG_MENU)
                g_ui.menu_prog = clampf((s_gbase + (float)(ty - s_gy0)) / (float)CURTAIN_TRAVEL, 0, 1);
            else if (s_gs == G_DRAG_NAV)
                g_ui.nav_x = clampf(s_gbase - (float)(tx - s_gx0), 0, (float)g_w);
        }

        if (release) {
            switch (s_gs) {
            case G_TOUCH:                                     // a tap (never classified)
                if (g_ui.nav_active ||
                    (g_ui.menu_prog > 0.003f && g_ui.menu_prog < 0.999f))
                    break;                                    // caught mid-flight: settled below
                if (g_ui.screen == UI_HOME && g_ui.menu_prog <= 0.003f && s_gy0 < 36)
                    menu_anim(true);                          // tap the status bar: pull down
                else {
                    int act = ui_hit_test(&g_ui, s_gx0, s_gy0);   // start point: no slop retarget
                    if (act) handle_action(act);
                }
                break;
            case G_DRAG_MENU: {
                bool opening = !g_ui.menu_open;
                bool commit = opening
                    ? (g_ui.menu_prog > COMMIT_FRAC || s_gvy > FLICK_PXPS)
                    : (g_ui.menu_prog < 1.0f - COMMIT_FRAC || s_gvy < -FLICK_PXPS);
                menu_anim(opening ? commit : !commit);
                break;
            }
            case G_DRAG_LIST: {                               // page the credential list
                int dy = s_gly - s_gy0;
                int dir = 0;
                if (dy < -LIST_PAGE_PX || s_gvy < -FLICK_PXPS) dir = 1;        // next page
                else if (dy > LIST_PAGE_PX || s_gvy > FLICK_PXPS) dir = -1;    // previous
                if (dir) {
                    int top = g_ui.cred_top + dir * CRED_VIS;
                    int max_top = g_ui.cred_n - 1;
                    if (top < 0) top = 0;
                    if (top > max_top) top = max_top;
                    top -= top % CRED_VIS;                    // page-aligned
                    if (top != g_ui.cred_top) { g_ui.cred_top = top; g_ui.cred_sel = top; }
                }
                break;
            }
            case G_DRAG_NAV: {
                bool to_creds = (s_gscreen == UI_HOME)
                    ? (g_ui.nav_x > (float)g_w * COMMIT_FRAC || s_gvx < -FLICK_PXPS)
                    : !(g_ui.nav_x < (float)g_w * (1.0f - COMMIT_FRAC) || s_gvx > FLICK_PXPS);
                if (to_creds) nav_to_creds_anim(NAV_SETTLE_MS);
                else          nav_to_home_anim(NAV_SETTLE_MS);
                break;
            }
            default:
                break;
            }
            // Never leave the strip or the sheet frozen mid-way after a lift.
            if (g_ui.nav_active && !s_tw_nav.active)
                tween_start(&s_tw_nav, g_ui.nav_x,
                            g_ui.screen == UI_CREDS ? (float)g_w : 0.0f, NAV_SETTLE_MS);
            if (!s_tw_menu.active && g_ui.menu_prog > 0.003f && g_ui.menu_prog < 0.999f)
                menu_anim(g_ui.menu_open);
            g_ui.menu_pressed = 0;
            s_gs = G_IDLE; s_release_miss = 0;
        }

        // Step the settle tweens; when the nav slide lands, drop the composite.
        if (s_tw_menu.active) g_ui.menu_prog = tween_val(&s_tw_menu, now);
        if (s_tw_nav.active) {
            g_ui.nav_x = tween_val(&s_tw_nav, now);
            if (!s_tw_nav.active) {
                g_ui.nav_active = false;
                g_ui.nav_x = g_ui.screen == UI_CREDS ? (float)g_w : 0.0f;
            }
        }

#if UI_CURTAIN_PEEK
        // One-shot curtain peek: ~500ms after first arriving home this boot the
        // sheet dips 12px and retracts — a wordless "pull me down" hint. A real
        // open (menu_anim) or any hard screen switch cancels/suppresses it.
        if (g_ui.screen != UI_HOME) s_home_since = now;
        else if (!s_peeked && !touched && (s_gs == G_IDLE || s_gs == G_DEAD) &&
                 !g_ui.nav_active && !g_ui.menu_open && !s_tw_menu.active &&
                 g_ui.menu_prog <= 0.003f && now - s_home_since >= 500) {
            s_peeked = true; s_peek_phase = 1;
            tween_start(&s_tw_menu, 0.0f, 12.0f / (float)CURTAIN_TRAVEL, 150);
        }
        if (s_peek_phase == 1 && !s_tw_menu.active) {
            s_peek_phase = 2;
            tween_start(&s_tw_menu, g_ui.menu_prog, 0.0f, 150);
        } else if (s_peek_phase == 2 && !s_tw_menu.active) {
            s_peek_phase = 0;
        }
#endif

        if (g_ui.screen != UI_HOME) menu_snap_closed();       // menu only lives on home
        if (g_ui.screen != UI_HOME && g_ui.screen != UI_CREDS) {
            g_ui.nav_active = false; g_ui.nav_x = 0; s_tw_nav.active = false;
        }

        // Idle auto-sleep: screen off after 1 min on a resting screen (not during an
        // approval/pairing). Deep sleep then follows from the sleep branch at 5 min.
        if ((g_ui.screen == UI_HOME || g_ui.screen == UI_LOCK || g_ui.screen == UI_CREDS)
            && (now - g_last_activity) >= SLEEP_IDLE_MS)
            enter_sleep();
#if BATT_DEMO
        { int ph = ((int)(g_ui.t * 6.0f)) % 200; g_ui.batt_pct = ph < 100 ? ph : 200 - ph; }  // placeholder sweep
#else
        {
            // Charging = active USB host (mounted && !suspended); the forced
            // VBUS-valid bits make tud_mounted() alone useless, see ck_usb_active.
            static int bframe;
            bool chg = ck_usb_active();
            if (chg != g_ui.batt_charging) {
                g_ui.batt_charging = chg;
                note_activity();          // plug/unplug wakes a slept screen
                bframe = 0;               // resample % now, not up to 2s stale
            }
            if (bframe++ % 40 == 0) g_ui.batt_pct = ck_batt_pct(chg);   // real cell, ~every 2s
        }
#endif
        // The RESULT screen is transient: fall back to the resting screen after a
        // few seconds so a finished op (or a stale CLI test) never lingers as a
        // fake-looking prompt. g_ui.t was reset to 0 when ui_result() set it.
        if (g_ui.screen == UI_RESULT && g_ui.t >= RESULT_TIMEOUT_S) goto_rest();
        snap = g_ui;
        strcpy(s_rp, g_rp); strcpy(s_sas, g_sas);
        strcpy(s_machine, g_machine); strcpy(s_msg, g_msg);
        {   // wall-clock t: frame-rate independent (clamped after sleeps/stalls)
            uint32_t dtms = now - s_prev_frame_ms;
            if (dtms > 100) dtms = 100;
            g_ui.t += (float)dtms / 1000.0f;
            s_prev_frame_ms = now;
        }
        bool anim = touched || s_gs != G_IDLE || s_tw_menu.active || s_tw_nav.active
                    || g_ui.nav_active || g_ui.screen == UI_BOOT;
        UNLOCK();
        snap.rp = s_rp; snap.sas = s_sas; snap.machine = s_machine; snap.msg = s_msg;

        ui_render(&cv, &snap);
        lcd_flush();
        // ~60 fps while a finger is down or something is animating, ~20 fps at rest.
        vTaskDelay(pdMS_TO_TICKS(anim ? 16 : 50));
    }
}

void ui_task_start(void)
{
    xTaskCreate(ui_task, "ck_ui", 6144, NULL, 4, NULL);
}
