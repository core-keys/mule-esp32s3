// core-keys M1 protocol mule — entry point.
//
// Brings up the composite USB device (TinyUSB over the ESP32-S3 native USB-OTG)
// and runs a single worker task that drains received HID reports and answers
// them. Received packets are queued from the TinyUSB callback so the USB task
// itself never blocks on our (blocking) sends. See DESIGN.md for the frozen v1
// topology this stands in for.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"
#include "soc/rtc_cntl_reg.h"
#include "tinyusb.h"
#include "corekeys.h"
#include "lcd.h"

static const char *TAG = "corekeys-mule";

// Descriptors and string table (usb_descriptors.c).
extern const tusb_desc_device_t desc_device;
extern const uint8_t desc_configuration[];
extern const char *string_desc_arr[];
extern const int   string_desc_arr_count;

// Vendor-channel control prefixes.
static const uint8_t REBOOT_DOWNLOAD_MAGIC[4] = { 0xC0, 0xDE, 0xB0, 0x07 };
static const uint8_t STATUS_QUERY_MAGIC[4]    = { 'S', 'T', 'A', 'T' };

// Force the ROM into serial download after reset (the bit lives in the always-on
// RTC domain, so it survives the system reset). Safe from any task context.
static void ck_enter_download(void)
{
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

// ---- RX queue: (instance, 64-byte report) from USB callback to worker --------
typedef struct {
    uint8_t itf;
    uint8_t data[CK_REPORT_SIZE];
} ck_rx_item_t;

static QueueHandle_t s_rx_queue;

void ck_usb_rx_enqueue(uint8_t itf, const uint8_t *pkt64)
{
    // Handle the reboot-to-download escape hatch HERE, in the USB task, before
    // queueing — so recovery works even if the worker task is wedged. (An
    // earlier build could hang the worker and take the escape hatch down with
    // it; this keeps the developer's only remote flash path independent of it.)
    if (itf == ITF_VENDOR && memcmp(pkt64, REBOOT_DOWNLOAD_MAGIC, 4) == 0) {
        ck_enter_download();
    }
    ck_rx_item_t item;
    item.itf = itf;
    memcpy(item.data, pkt64, CK_REPORT_SIZE);
    // TinyUSB callbacks run in task (not ISR) context: enqueue without blocking
    // and drop on overflow rather than stalling the USB task.
    if (xQueueSend(s_rx_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "rx queue full; dropping report");
    }
}

// ---- Blocking report send (worker task only) --------------------------------
void ck_report_send(uint8_t itf, const uint8_t *report64)
{
    // Wait for the endpoint to accept a new report, yielding to let the USB
    // task run. Bounded so a stalled host cannot wedge the worker forever.
    for (int i = 0; i < 200; i++) {
        if (tud_mounted() && tud_hid_n_ready(itf)) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (tud_mounted()) {
        tud_hid_n_report(itf, 0, report64, CK_REPORT_SIZE);
    }
}

// ---- Vendor channel (ITF_VENDOR) ---------------------------------------------
// The reboot-to-download escape hatch (magic C0 DE B0 07) is handled up in
// ck_usb_rx_enqueue so it survives a hung worker. Here, on the worker task, we
// answer the STATUS query (remote diagnostics — there is no serial console once
// TinyUSB owns the USB) and otherwise echo (M1 loopback), so the daemon side
// can be built against real composite-USB framing before the co-auth protocol
// lands (M2/M3).
void vendor_rx_packet(const uint8_t *pkt64)
{
    if (memcmp(pkt64, STATUS_QUERY_MAGIC, 4) == 0) {
        // Remote diagnostics: report LCD bring-up result (no serial console).
        lcd_diag_t d = lcd_diag();
        uint8_t r[CK_REPORT_SIZE];
        memset(r, 0, sizeof(r));
        memcpy(r, STATUS_QUERY_MAGIC, 4);
        r[4] = d.lcd_ok;
        r[5] = d.fb_internal;
        memcpy(&r[6],  &d.fb_bytes,      4);
        memcpy(&r[10], &d.free_heap,     4);
        memcpy(&r[14], &d.free_internal, 4);
        ck_report_send(ITF_VENDOR, r);
        return;
    }
    ck_report_send(ITF_VENDOR, pkt64);   // loopback
}

// ---- Worker task -------------------------------------------------------------
static void worker_task(void *arg)
{
    (void)arg;
    // Let the boot splash linger, then show the idle status screen. All UI
    // drawing happens on this task (plus the pre-worker splash), so there is
    // never concurrent access to the framebuffer.
    vTaskDelay(pdMS_TO_TICKS(1800));
    ui_note_ctap("idle");

    ck_rx_item_t item;
    for (;;) {
        if (xQueueReceive(s_rx_queue, &item, portMAX_DELAY) == pdTRUE) {
            if (item.itf == ITF_CTAP)        ctaphid_rx_packet(item.data);
            else if (item.itf == ITF_VENDOR) vendor_rx_packet(item.data);
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "core-keys M1 mule starting");

    s_rx_queue = xQueueCreate(16, sizeof(ck_rx_item_t));
    if (!s_rx_queue) { ESP_LOGE(TAG, "queue alloc failed"); return; }

    const tinyusb_config_t tusb_cfg = {
        .device_descriptor        = &desc_device,
        .string_descriptor        = string_desc_arr,
        .string_descriptor_count  = string_desc_arr_count,
        .external_phy             = false,
        .configuration_descriptor = desc_configuration,
    };
    // Bring the LCD up FIRST, before TinyUSB claims the native USB. Two reasons:
    // the boot splash appears immediately on power-on, and until TinyUSB installs
    // the console still owns USB-Serial-JTAG, so lcd_init()'s result (and any
    // fault) is observable over the serial console for debugging.
    ESP_LOGI(TAG, "boot: free heap=%u", (unsigned)esp_get_free_heap_size());
    bool lcd_ok = lcd_init();
    ESP_LOGI(TAG, "boot: lcd_init()=%d", lcd_ok);
    if (lcd_ok) ui_boot_splash();

    // Brief dwell so the splash is visible and the pre-USB log window stays open.
    vTaskDelay(pdMS_TO_TICKS(3000));

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG, "USB composite device installed (CTAP + vendor HID)");

    xTaskCreate(worker_task, "ck_worker", 6144, NULL, 5, NULL);
    ESP_LOGI(TAG, "worker task running; awaiting host");
}
