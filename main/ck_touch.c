// Capacitive touch driver. See ck_touch.h.
#include "ck_touch.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "ck_touch";

#define PIN_SCL 17
#define PIN_SDA 18
#define PIN_INT 16
#define PIN_RST 21
#define ADDR_CST816 0x15
#define ADDR_CST328 0x1A

// Panel geometry (matches lcd.c): 170 x 320 portrait.
#define DISP_W 170
#define DISP_H 320

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static uint8_t s_addr;

uint8_t ck_touch_addr(void) { return s_addr; }

static void reset_pulse(void)
{
    gpio_config_t io = { .pin_bit_mask = 1ULL << PIN_RST, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io);
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));   // CST816 boots in ~50-100ms
    gpio_config_t in = { .pin_bit_mask = 1ULL << PIN_INT, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&in);
}

static int reg_read(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 50) == ESP_OK ? 0 : -1;
}
static int reg_write(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, 2, 50) == ESP_OK ? 0 : -1;
}

uint8_t ck_touch_init(void)
{
    reset_pulse();

    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = PIN_SCL,
        .sda_io_num = PIN_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK) {
        ESP_LOGW(TAG, "i2c bus init failed");
        return 0;
    }

    s_addr = 0;
    if (i2c_master_probe(s_bus, ADDR_CST816, 50) == ESP_OK) s_addr = ADDR_CST816;
    else if (i2c_master_probe(s_bus, ADDR_CST328, 50) == ESP_OK) s_addr = ADDR_CST328;

    if (!s_addr) { ESP_LOGW(TAG, "no touch controller found on 0x15/0x1A"); return 0; }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = s_addr,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev) != ESP_OK) { s_addr = 0; return 0; }

    if (s_addr == ADDR_CST816) {
        reg_write(0xFE, 0x01);        // DisAutoSleep: required for polling (LilyGO note)
    }
    ESP_LOGI(TAG, "touch controller at 0x%02x (%s)", s_addr,
             s_addr == ADDR_CST816 ? "CST816" : "CST328");
    return s_addr;
}

// Map the controller's raw coordinates to display space. Start 1:1; the on-device
// calibration overlay tells us if an axis needs flipping (fixed in the UI build).
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static void map_xy(int rx, int ry, int *x, int *y)
{
    // Leave headroom below the display (y up to 359) so the caller can tell the
    // capacitive home button (physically below the panel, ry ~320-360) from a
    // tap on the bottom edge of the display.
    *x = clampi(rx, 0, DISP_W - 1);
    *y = clampi(ry, 0, 359);
}

bool ck_touch_read(int *x, int *y, uint8_t *gesture)
{
    if (!s_addr) return false;

    if (s_addr == ADDR_CST816) {
        uint8_t b[6];
        if (reg_read(0x01, b, 6) != 0) return false;
        uint8_t gest = b[0], fingers = b[1];
        if (fingers == 0) return false;
        int rx = ((b[2] & 0x0F) << 8) | b[3];
        int ry = ((b[4] & 0x0F) << 8) | b[5];
        if (gesture) *gesture = gest;
        map_xy(rx, ry, x, y);
        return true;
    }

    // CST328: multi-touch, 16-bit register addresses — read the first point.
    if (s_addr == ADDR_CST328) {
        uint8_t reg[2] = { 0xD0, 0x00 };    // touch status / first point block
        uint8_t b[7];
        if (i2c_master_transmit_receive(s_dev, reg, 2, b, sizeof(b), 50) != ESP_OK) return false;
        uint8_t num = b[0] & 0x0F;
        if (num == 0 || num > 5) return false;
        int rx = ((int)b[1] << 4) | (b[3] >> 4);
        int ry = ((int)b[2] << 4) | (b[3] & 0x0F);
        if (gesture) *gesture = 0;
        map_xy(rx, ry, x, y);
        return true;
    }
    return false;
}
