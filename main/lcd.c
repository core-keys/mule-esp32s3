// core-keys M1 mule — ST7789 over the ESP32-S3 i80 parallel bus (T-Display-S3).
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "lcd.h"
#include "font8x16.h"

// ---- T-Display-S3 pin map (verified against LilyGO pin_config.h) -------------
#define PIN_POWER 15   // PWR_EN — MUST be high or the panel stays dark
#define PIN_BL    38   // backlight
#define PIN_RD     9   // read strobe — held high (write-only bus)
#define PIN_DC     7
#define PIN_WR     8
#define PIN_CS     6
#define PIN_RST    5
static const int PIN_DATA[8] = { 39, 40, 41, 42, 45, 46, 47, 48 };

#define X_GAP 35       // 170-wide panel offset within 240-wide ST7789 RAM

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb;
static bool      s_ok;
static bool      s_fb_internal;

// ST7789 takes big-endian RGB565; we set swap_color_bytes so we can store native.
uint16_t lcd_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

bool lcd_init(void)
{
    // Enable panel power and backlight.
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_POWER) | (1ULL << PIN_BL) | (1ULL << PIN_RD),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(PIN_POWER, 1);
    gpio_set_level(PIN_RD, 1);
    gpio_set_level(PIN_BL, 1);

    esp_lcd_i80_bus_handle_t bus = NULL;
    esp_lcd_i80_bus_config_t bus_cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .dc_gpio_num = PIN_DC,
        .wr_gpio_num = PIN_WR,
        .bus_width = 8,
        .max_transfer_bytes = LCD_W * LCD_H * sizeof(uint16_t) + 16,
        .dma_burst_size = 64,
    };
    for (int i = 0; i < 8; i++) bus_cfg.data_gpio_nums[i] = PIN_DATA[i];
    if (esp_lcd_new_i80_bus(&bus_cfg, &bus) != ESP_OK) return false;

    esp_lcd_panel_io_handle_t pio = NULL;
    esp_lcd_panel_io_i80_config_t pio_cfg = {
        .cs_gpio_num = PIN_CS,
        .pclk_hz = 10 * 1000 * 1000,
        .trans_queue_depth = 10,
        .dc_levels = { .dc_idle_level = 0, .dc_cmd_level = 0, .dc_dummy_level = 0, .dc_data_level = 1 },
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags = { .swap_color_bytes = 1 },
    };
    if (esp_lcd_new_panel_io_i80(bus, &pio_cfg, &pio) != ESP_OK) return false;

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_st7789(pio, &panel_cfg, &s_panel) != ESP_OK) return false;

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);   // T-Display-S3 needs inversion on
    esp_lcd_panel_set_gap(s_panel, X_GAP, 0);
    esp_lcd_panel_disp_on_off(s_panel, true);

    // Framebuffer: prefer PSRAM, fall back to internal DMA RAM.
    size_t bytes = LCD_W * LCD_H * sizeof(uint16_t);
    s_fb = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    s_fb_internal = false;
    if (!s_fb) { s_fb = heap_caps_malloc(bytes, MALLOC_CAP_DMA); s_fb_internal = true; }
    if (!s_fb) return false;

    s_ok = true;
    lcd_fill(lcd_rgb(0x0E, 0x14, 0x1C));
    lcd_flush();
    return true;
}

void lcd_fill(uint16_t color)
{
    if (!s_fb) return;
    for (int i = 0; i < LCD_W * LCD_H; i++) s_fb[i] = color;
}

// Draw one glyph scaled by an integer factor.
static void draw_char(int x, int y, char c, int scale, uint16_t fg, uint16_t bg)
{
    if (!s_fb) return;
    if (c < FONT_FIRST || c > FONT_LAST) c = '?';
    const uint8_t *g = font8x16[(int)c - FONT_FIRST];
    for (int row = 0; row < FONT_H; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < FONT_W; col++) {
            uint16_t color = (bits & (0x80 >> col)) ? fg : bg;
            for (int sy = 0; sy < scale; sy++) {
                int py = y + row * scale + sy;
                if (py < 0 || py >= LCD_H) continue;
                for (int sx = 0; sx < scale; sx++) {
                    int px = x + col * scale + sx;
                    if (px < 0 || px >= LCD_W) continue;
                    s_fb[py * LCD_W + px] = color;
                }
            }
        }
    }
}

void lcd_text(int x, int y, const char *s, int scale, uint16_t fg, uint16_t bg)
{
    int cx = x;
    for (; *s; s++) {
        draw_char(cx, y, *s, scale, fg, bg);
        cx += FONT_W * scale;
    }
}

void lcd_flush(void)
{
    if (s_ok) esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_W, LCD_H, s_fb);
}

lcd_diag_t lcd_diag(void)
{
    lcd_diag_t d = {
        .lcd_ok = s_ok ? 1 : 0,
        .fb_internal = s_fb_internal ? 1 : 0,
        .fb_bytes = LCD_W * LCD_H * (uint32_t)sizeof(uint16_t),
        .free_heap = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
        .free_internal = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
    };
    return d;
}

// ---- Screens -----------------------------------------------------------------
#define COL_BG    lcd_rgb(0x0E, 0x14, 0x1C)   // near-black navy (matches design)
#define COL_FG    lcd_rgb(0xE3, 0xE7, 0xEB)   // off-white
#define COL_MUTED lcd_rgb(0x6E, 0x7A, 0x86)
#define COL_COPPER lcd_rgb(0xD9, 0x8F, 0x51)  // the "device" accent from the study
#define COL_TEAL   lcd_rgb(0x56, 0xB4, 0xC4)  // the "desktop" accent

static int centered_x(const char *s, int scale) { return (LCD_W - (int)strlen(s) * FONT_W * scale) / 2; }

void ui_boot_splash(void)
{
    lcd_fill(COL_BG);
    lcd_text(centered_x("core", 2), 96,  "core", 2, COL_COPPER, COL_BG);
    lcd_text(centered_x("-keys", 2), 128, "-keys", 2, COL_COPPER, COL_BG);
    lcd_text(centered_x("M1 mule", 1), 170, "M1 mule", 1, COL_FG, COL_BG);
    lcd_text(centered_x("SSH + FIDO2", 1), 196, "SSH + FIDO2", 1, COL_TEAL, COL_BG);
    lcd_text(centered_x("split-key", 1), 214, "split-key", 1, COL_MUTED, COL_BG);
    lcd_flush();
}

static char s_last_cmd[20] = "idle";
static uint32_t s_req_count;

void ui_note_ctap(const char *cmd)
{
    if (cmd) { strncpy(s_last_cmd, cmd, sizeof(s_last_cmd) - 1); s_last_cmd[sizeof(s_last_cmd)-1] = 0; s_req_count++; }
    char line[40];
    lcd_fill(COL_BG);
    lcd_text(centered_x("core-keys", 2), 20, "core-keys", 2, COL_COPPER, COL_BG);
    lcd_text(8, 64, "state  ready", 1, COL_FG, COL_BG);
    snprintf(line, sizeof(line), "last   %s", s_last_cmd);
    lcd_text(8, 88, line, 1, COL_FG, COL_BG);
    snprintf(line, sizeof(line), "reqs   %lu", (unsigned long)s_req_count);
    lcd_text(8, 112, line, 1, COL_FG, COL_BG);
    lcd_text(8, 288, "awaiting host", 1, COL_MUTED, COL_BG);
    lcd_flush();
}
