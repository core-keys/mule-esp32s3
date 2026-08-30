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

// The framebuffer is now drawn by the UI task (ui.c) via ui_draw; lcd.c only
// brings up the panel, hands out the buffer, and flushes it.
uint16_t *lcd_fb(void) { return s_fb; }

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

