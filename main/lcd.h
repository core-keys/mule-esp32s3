// core-keys M1 mule — ST7789 LCD driver + minimal UI (LilyGO T-Display-S3).
//
// The display is the WYSIWYS approval surface the design makes mandatory: a
// button press must approve *this* operation, shown on a screen the device
// controls, not a blind touch. M1 proves the panel and text pipeline; the
// per-operation approval prompts arrive with makeCredential/getAssertion (M2).
#pragma once
#include <stdint.h>
#include <stdbool.h>

// Panel geometry (portrait: USB at the bottom). 170x320 visible; the ST7789
// RAM is 240 wide, so the panel sits at column offset 35.
#define LCD_W 170
#define LCD_H 320

bool     lcd_init(void);                 // returns true if the panel came up
uint16_t lcd_rgb(uint8_t r, uint8_t g, uint8_t b);
void     lcd_fill(uint16_t color);
uint16_t *lcd_fb(void);                  // the RGB565 framebuffer (owned by ui.c)
void     lcd_flush(void);                // push the framebuffer to the panel

// Remote diagnostics (there is no serial console once TinyUSB owns the USB).
typedef struct {
    uint8_t  lcd_ok;
    uint8_t  fb_internal;   // 1 = framebuffer in internal RAM, 0 = PSRAM
    uint32_t fb_bytes;
    uint32_t free_heap;
    uint32_t free_internal;
} lcd_diag_t;
lcd_diag_t lcd_diag(void);

// The screen API (ui_boot_splash / ui_note_ctap / ui_approval / ui_result /
// ui_enroll / ui_pair_sas) now lives in ui.h — those are UI-task state setters.
