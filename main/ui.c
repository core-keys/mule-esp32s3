// core-keys device UI task. See ui.h.
#include "ui.h"
#include "ui_screens.h"
#include "lcd.h"
#include "ck_store.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

// Shared UI state, guarded by g_mutex. String fields live in these buffers so a
// setter's pointer never dangles; the task deep-copies them into a private
// snapshot under the lock, then renders without holding it.
static ui_state_t g_ui;
static char g_rp[48], g_sas[12], g_machine[40], g_msg[40];
static SemaphoreHandle_t g_mutex;
static uint16_t *g_fb;
static int g_w, g_h;

#define LOCK()   xSemaphoreTake(g_mutex, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(g_mutex)

void ui_init(uint16_t *fb, int w, int h)
{
    g_fb = fb; g_w = w; g_h = h;
    g_mutex = xSemaphoreCreateMutex();
    memset(&g_ui, 0, sizeof g_ui);
    g_ui.screen = UI_BOOT;
    g_ui.pin_max = 4;
    g_rp[0] = g_sas[0] = g_machine[0] = g_msg[0] = 0;
    g_ui.rp = g_rp; g_ui.sas = g_sas; g_ui.machine = g_machine; g_ui.msg = g_msg;
}

// ---- state setters (any task) -----------------------------------------------
void ui_boot_splash(void)
{
    LOCK(); g_ui.screen = UI_BOOT; g_ui.t = 0; UNLOCK();
}

void ui_note_ctap(const char *cmd)
{
    (void)cmd;                        // the specific activity isn't shown; go home
    int d = ck_store_auth_count();
    LOCK();
    g_ui.screen = UI_HOME;
    g_ui.paired = d > 0; g_ui.daemon_count = d;
    g_ui.cred_count = 0;              // journal lands in the next build
    g_ui.unlocked = true;            // PIN lock lands in the next build
    UNLOCK();
}

void ui_approval(const char *who, bool forwarded)
{
    LOCK();
    snprintf(g_rp, sizeof g_rp, "%s", who ? who : "");
    g_ui.forwarded = forwarded; g_ui.coauthd = false; g_ui.progress = 0;
    g_ui.screen = UI_APPROVE;
    UNLOCK();
}

void ui_result(const char *msg)
{
    LOCK();
    snprintf(g_msg, sizeof g_msg, "%s", msg ? msg : "");
    g_ui.screen = UI_RESULT; g_ui.t = 0;
    UNLOCK();
}

void ui_enroll(void)
{
    LOCK();
    g_sas[0] = 0;                     // no SAS yet -> "waiting for desktop"
    g_machine[0] = 0;
    g_ui.screen = UI_PAIR; g_ui.t = 0;
    UNLOCK();
}

void ui_pair_sas(uint32_t sas, const uint8_t *machine, uint16_t machine_len)
{
    LOCK();
    snprintf(g_sas, sizeof g_sas, "%06lu", (unsigned long)(sas % 1000000UL));
    uint16_t n = machine_len < sizeof(g_machine) - 1 ? machine_len : sizeof(g_machine) - 1;
    memcpy(g_machine, machine, n); g_machine[n] = 0;
    g_ui.screen = UI_PAIR;
    UNLOCK();
}

// ---- the render task --------------------------------------------------------
static char s_rp[48], s_sas[12], s_machine[40], s_msg[40];

static void ui_task(void *arg)
{
    (void)arg;
    ui_canvas_t cv = { g_fb, g_w, g_h };
    ui_state_t snap;
    for (;;) {
        LOCK();
        snap = g_ui;
        strcpy(s_rp, g_rp); strcpy(s_sas, g_sas);
        strcpy(s_machine, g_machine); strcpy(s_msg, g_msg);
        g_ui.t += 0.05f;             // advance animation clock
        UNLOCK();
        snap.rp = s_rp; snap.sas = s_sas; snap.machine = s_machine; snap.msg = s_msg;

        ui_render(&cv, &snap);
        lcd_flush();
        vTaskDelay(pdMS_TO_TICKS(50));   // ~20 fps
    }
}

void ui_task_start(void)
{
    xTaskCreate(ui_task, "ck_ui", 6144, NULL, 4, NULL);
}
