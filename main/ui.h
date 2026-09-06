// core-keys device UI task: sole owner of the LCD framebuffer. Any task can call
// the screen functions below — they only update state; the UI task renders +
// flushes (with animation) on its own. Keeps the ring/glow drawing off the
// worker task so co-auth timing is unaffected.
#pragma once
#include <stdint.h>
#include <stdbool.h>

// Give the UI the LCD framebuffer + dimensions (from lcd_init), then start it.
void ui_init(uint16_t *fb, int w, int h);
void ui_task_start(void);

// Lock: set the initial lock state from NVS (call once after ck_store is up).
// If a PIN is set the device boots locked; the UI task handles unlocking.
void ui_lock_boot(void);
// The gate every crypto operation must check: false = locked (refuse, no button).
bool ui_is_unlocked(void);

// True once if the on-screen APPROVE / MATCH button was tapped since the last
// check. A waiting worker (co-auth / SSH / pairing) polls this like a BOOT press.
bool ui_touch_approve_taken(void);

// True while an active USB host is attached — proxy for external power/charging
// (a data-less wall charger reads false). Defined in main.c.
bool ck_usb_active(void);

// Screen API — same names the firmware already calls (now state setters).
void ui_boot_splash(void);
void ui_note_ctap(const char *cmd);                 // activity -> home
void ui_approval(const char *who, bool forwarded);  // WYSIWYS approve screen
void ui_result(const char *msg);                    // brief post-decision screen
void ui_enroll(void);                               // pairing: waiting for desktop
void ui_pair_sas(uint32_t sas, const uint8_t *machine, uint16_t machine_len);
