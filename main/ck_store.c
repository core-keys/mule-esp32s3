// Persistent key store over NVS. See ck_store.h.
#include "ck_store.h"
#include <stdio.h>
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_random.h"
#include "esp_log.h"

static const char *TAG = "ck_store";
static const char *NS = "corekeys";

// From the noise-c donna backend (linked): X25519 scalar mult.
extern int curve25519_donna(uint8_t *pub, const uint8_t *priv, const uint8_t *base);

void ck_store_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // Partition needs an erase (first use / layout change).
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);
}

int ck_store_device_static(uint8_t priv[32], uint8_t pub[32])
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;

    size_t len = 32;
    if (nvs_get_blob(h, "dev_priv", priv, &len) == ESP_OK && len == 32) {
        len = 32;
        if (nvs_get_blob(h, "dev_pub", pub, &len) != ESP_OK || len != 32) {
            nvs_close(h);
            return -1;
        }
        nvs_close(h);
        return 0;
    }

    // First boot: generate a clamped X25519 static and persist it.
    esp_fill_random(priv, 32);
    priv[0] &= 248;
    priv[31] &= 127;
    priv[31] |= 64;
    static const uint8_t base9[32] = { 9 };
    curve25519_donna(pub, priv, base9);

    nvs_set_blob(h, "dev_priv", priv, 32);
    nvs_set_blob(h, "dev_pub", pub, 32);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "generated a fresh device identity");
    return 0;
}

int ck_store_auth_count(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return 0;
    uint8_t cnt = 0;
    nvs_get_u8(h, "auth_cnt", &cnt);
    nvs_close(h);
    return cnt > CK_MAX_AUTH_DAEMONS ? CK_MAX_AUTH_DAEMONS : cnt;
}

int ck_store_auth_get(int idx, uint8_t pub[32])
{
    if (idx < 0 || idx >= CK_MAX_AUTH_DAEMONS) return -1;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return -1;
    char key[16];
    snprintf(key, sizeof(key), "auth_%d", idx);
    size_t len = 32;
    esp_err_t e = nvs_get_blob(h, key, pub, &len);
    nvs_close(h);
    return (e == ESP_OK && len == 32) ? 0 : -1;
}

int ck_store_auth_add(const uint8_t pub[32])
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    uint8_t cnt = 0;
    nvs_get_u8(h, "auth_cnt", &cnt);
    if (cnt > CK_MAX_AUTH_DAEMONS) cnt = CK_MAX_AUTH_DAEMONS;

    for (int i = 0; i < cnt; i++) {
        char key[16];
        snprintf(key, sizeof(key), "auth_%d", i);
        uint8_t existing[32];
        size_t len = 32;
        if (nvs_get_blob(h, key, existing, &len) == ESP_OK && len == 32 &&
            memcmp(existing, pub, 32) == 0) {
            nvs_close(h);
            return 0; // already pinned
        }
    }
    if (cnt >= CK_MAX_AUTH_DAEMONS) { nvs_close(h); return -1; }

    char key[16];
    snprintf(key, sizeof(key), "auth_%d", cnt);
    nvs_set_blob(h, key, pub, 32);
    nvs_set_u8(h, "auth_cnt", (uint8_t)(cnt + 1));
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "pinned daemon #%d", cnt);
    return 0;
}

int ck_store_auth_clear(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    nvs_set_u8(h, "auth_cnt", 0);
    nvs_commit(h);
    nvs_close(h);
    return 0;
}
