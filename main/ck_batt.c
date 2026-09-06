// Battery fuel gauge. See ck_batt.h.
//
// T-Display-S3: the LiPo on the JST connector is sensed through an on-board 2:1
// divider on GPIO4 = ADC1 channel 3. We read the divided voltage, calibrate it to
// millivolts, double it to recover VBAT, then map VBAT to a state-of-charge %
// with a small LiPo discharge LUT. If your battery is wired to a different pin or
// divider, adjust BAT_ADC_CHAN / BAT_DIVIDER below.
#include "ck_batt.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

static const char *TAG = "ck_batt";

#define BAT_ADC_UNIT   ADC_UNIT_1
#define BAT_ADC_CHAN   ADC_CHANNEL_3        // GPIO4 on ESP32-S3
#define BAT_ATTEN      ADC_ATTEN_DB_12      // ~0..3.1V measurable (pin sees VBAT/2)
#define BAT_DIVIDER    2                    // on-board 2:1 resistor divider

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_ready, s_cali_ok;

void ck_batt_init(void)
{
    adc_oneshot_unit_init_cfg_t u = { .unit_id = BAT_ADC_UNIT };
    if (adc_oneshot_new_unit(&u, &s_adc) != ESP_OK) { ESP_LOGW(TAG, "adc unit init failed"); return; }
    adc_oneshot_chan_cfg_t c = { .atten = BAT_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_oneshot_config_channel(s_adc, BAT_ADC_CHAN, &c) != ESP_OK) { ESP_LOGW(TAG, "adc chan cfg failed"); return; }

    adc_cali_curve_fitting_config_t cc = {
        .unit_id = BAT_ADC_UNIT, .chan = BAT_ADC_CHAN,
        .atten = BAT_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    s_cali_ok = adc_cali_create_scheme_curve_fitting(&cc, &s_cali) == ESP_OK;
    s_ready = true;
    ESP_LOGI(TAG, "battery ADC up (cali=%d)", s_cali_ok);
}

// Resting single-cell LiPo discharge curve (mV -> %). Interpolated between points.
static int mv_to_pct(int vbat)
{
    static const int lut[][2] = {
        { 4200, 100 }, { 4100, 90 }, { 4000, 80 }, { 3900, 65 }, { 3800, 55 },
        { 3700, 40 }, { 3600, 25 }, { 3500, 12 }, { 3400, 5 }, { 3300, 0 },
    };
    const int N = sizeof(lut) / sizeof(lut[0]);
    if (vbat >= lut[0][0]) return 100;
    if (vbat <= lut[N - 1][0]) return 0;
    for (int i = 0; i < N - 1; i++) {
        if (vbat <= lut[i][0] && vbat > lut[i + 1][0]) {
            int v1 = lut[i][0], p1 = lut[i][1], v0 = lut[i + 1][0], p0 = lut[i + 1][1];
            return p0 + (p1 - p0) * (vbat - v0) / (v1 - v0);
        }
    }
    return 0;
}

// While the TP4065 is charging, VBAT sits above the cell's resting voltage by the
// IR drop of the charge current. That current is roughly constant in the CC phase
// and tapers to ~0 as VBAT approaches 4.2V (CV phase), so the bias is modelled as
// a full offset that tapers linearly to zero at 4.2V — a *fixed* offset would cap
// a full, still-plugged cell at ~82% forever.
#define CHG_OFFSET_MV  160
#define CHG_TAPER_MV   300

static int chg_offset(int vbat)
{
    int d = 4200 - vbat;
    if (d <= 0) return 0;
    if (d >= CHG_TAPER_MV) return CHG_OFFSET_MV;
    return CHG_OFFSET_MV * d / CHG_TAPER_MV;
}

int ck_batt_pct(bool charging)
{
    if (!s_ready) return -1;
    int acc = 0, n = 0;
    for (int i = 0; i < 8; i++) {
        int raw;
        if (adc_oneshot_read(s_adc, BAT_ADC_CHAN, &raw) != ESP_OK) continue;
        int mv;
        if (s_cali_ok) {
            if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) continue;
        } else {
            mv = raw * 3100 / 4095;           // rough fallback if calibration is absent
        }
        acc += mv; n++;
    }
    if (!n) return -1;
    int vbat = (acc / n) * BAT_DIVIDER;
    if (charging) vbat -= chg_offset(vbat);
    return mv_to_pct(vbat);
}
