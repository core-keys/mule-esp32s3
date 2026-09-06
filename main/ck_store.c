// Persistent key store over NVS. See ck_store.h.
#include "ck_store.h"
#include <stdio.h>
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_random.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"

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

// ---- Unlock PIN --------------------------------------------------------------
static void pin_hash(const uint8_t salt[16], const char *pin, uint8_t out[32])
{
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    mbedtls_sha256_update(&c, salt, 16);
    mbedtls_sha256_update(&c, (const uint8_t *)pin, strlen(pin));
    mbedtls_sha256_finish(&c, out);
    mbedtls_sha256_free(&c);
}

bool ck_store_has_pin(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t hash[32]; size_t len = 32;
    bool has = (nvs_get_blob(h, "pin_hash", hash, &len) == ESP_OK && len == 32);
    nvs_close(h);
    return has;
}

int ck_store_pin_set(const char *pin)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    uint8_t salt[16], hash[32];
    esp_fill_random(salt, 16);
    pin_hash(salt, pin, hash);
    nvs_set_blob(h, "pin_salt", salt, 16);
    nvs_set_blob(h, "pin_hash", hash, 32);
    nvs_set_u8(h, "pin_retries", 0);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "unlock PIN set");
    return 0;
}

int ck_store_pin_retries_left(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return 0;
    uint8_t r = 0;
    nvs_get_u8(h, "pin_retries", &r);
    nvs_close(h);
    return r >= CK_PIN_MAX_RETRIES ? 0 : CK_PIN_MAX_RETRIES - r;
}

int ck_store_pin_verify(const char *pin)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return 1;
    uint8_t salt[16], want[32], got[32];
    size_t sl = 16, wl = 32;
    uint8_t r = 0;
    nvs_get_u8(h, "pin_retries", &r);
    if (r >= CK_PIN_MAX_RETRIES) { nvs_close(h); return -1; }
    if (nvs_get_blob(h, "pin_salt", salt, &sl) != ESP_OK ||
        nvs_get_blob(h, "pin_hash", want, &wl) != ESP_OK) { nvs_close(h); return 1; }
    pin_hash(salt, pin, got);
    int diff = 0;
    for (int i = 0; i < 32; i++) diff |= got[i] ^ want[i];
    if (diff == 0) {
        nvs_set_u8(h, "pin_retries", 0);
        nvs_commit(h); nvs_close(h);
        return 0;
    }
    nvs_set_u8(h, "pin_retries", (uint8_t)(r + 1));
    nvs_commit(h); nvs_close(h);
    return 1;
}

int ck_store_pin_clear(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    nvs_erase_key(h, "pin_hash");
    nvs_erase_key(h, "pin_salt");
    nvs_set_u8(h, "pin_retries", 0);
    nvs_commit(h);
    nvs_close(h);
    return 0;
}

// ---- Credential journal ------------------------------------------------------
typedef struct { char rp[40]; char user[40]; uint8_t fp[4]; } cred_ent_t;

int ck_store_cred_count(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return 0;
    uint8_t c = 0;
    nvs_get_u8(h, "cred_cnt", &c);
    nvs_close(h);
    return c > CK_CRED_MAX ? CK_CRED_MAX : c;
}

int ck_store_cred_del(int idx)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    uint8_t cnt = 0;
    nvs_get_u8(h, "cred_cnt", &cnt);
    if (cnt > CK_CRED_MAX) cnt = CK_CRED_MAX;
    if (idx < 0 || idx >= cnt) { nvs_close(h); return -1; }

    // Shift every entry after idx down one slot, so indices stay contiguous.
    for (int i = idx; i < cnt - 1; i++) {
        char ka[24], kb[24];
        snprintf(ka, sizeof(ka), "cred_%d", i);
        snprintf(kb, sizeof(kb), "cred_%d", i + 1);
        cred_ent_t e; size_t len = sizeof(e);
        if (nvs_get_blob(h, kb, &e, &len) == ESP_OK && len == sizeof(e))
            nvs_set_blob(h, ka, &e, sizeof(e));
    }
    char last[24]; snprintf(last, sizeof(last), "cred_%d", cnt - 1);
    nvs_erase_key(h, last);
    cnt--;
    nvs_set_u8(h, "cred_cnt", cnt);
    nvs_commit(h);
    nvs_close(h);
    return 0;
}

int ck_store_cred_get(int idx, char *rp, int rp_cap, char *user, int user_cap)
{
    if (idx < 0 || idx >= CK_CRED_MAX) return -1;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return -1;
    char key[16]; snprintf(key, sizeof(key), "cred_%d", idx);
    cred_ent_t e; size_t len = sizeof(e);
    esp_err_t r = nvs_get_blob(h, key, &e, &len);
    nvs_close(h);
    if (r != ESP_OK || len != sizeof(e)) return -1;
    snprintf(rp, rp_cap, "%s", e.rp);
    snprintf(user, user_cap, "%s", e.user);
    return 0;
}

int ck_store_cred_add(const char *rp, const char *user, const uint8_t *cred_id, int cred_len)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    uint8_t cnt = 0;
    nvs_get_u8(h, "cred_cnt", &cnt);
    if (cnt > CK_CRED_MAX) cnt = CK_CRED_MAX;

    cred_ent_t e;
    memset(&e, 0, sizeof(e));
    snprintf(e.rp, sizeof(e.rp), "%s", rp ? rp : "");
    snprintf(e.user, sizeof(e.user), "%s", (user && user[0]) ? user : "-");
    uint8_t d[32];
    mbedtls_sha256(cred_id, cred_len, d, 0);
    memcpy(e.fp, d, 4);

    // Dedup by rp: update in place if the site is already listed.
    int slot = -1;
    for (int i = 0; i < cnt; i++) {
        char key[16]; snprintf(key, sizeof(key), "cred_%d", i);
        cred_ent_t x; size_t len = sizeof(x);
        if (nvs_get_blob(h, key, &x, &len) == ESP_OK && len == sizeof(x) &&
            strncmp(x.rp, e.rp, sizeof(e.rp)) == 0) { slot = i; break; }
    }
    if (slot < 0) {
        if (cnt >= CK_CRED_MAX) { nvs_close(h); return -1; }  // full: keep first 8
        slot = cnt; cnt++;
    }
    char key[16]; snprintf(key, sizeof(key), "cred_%d", slot);
    nvs_set_blob(h, key, &e, sizeof(e));
    nvs_set_u8(h, "cred_cnt", cnt);
    nvs_commit(h);
    nvs_close(h);
    return 0;
}

// ---- Per-site icon store -----------------------------------------------------
// Keyed by a hash of the rp so it is independent of credential slots. Key is
// "ico_XXXXXXXX" (first 8 hex chars of SHA-256(rp)). Blob = [w, h, pixels].
#define CK_ICON_MAX_PIX  (32 * 32 * 2)          // w,h <= 32 => at most 2048 bytes

static void icon_key(const uint8_t *rp, int rp_len, char out[24])
{
    uint8_t d[32];
    mbedtls_sha256(rp, rp_len, d, 0);
    snprintf(out, 24, "ico_%02x%02x%02x%02x", d[0], d[1], d[2], d[3]);
}

int ck_store_icon_set(const char *rp, int rp_len, uint8_t w, uint8_t h, const uint8_t *pixels)
{
    if (w < 1 || w > 32 || h < 1 || h > 32) return -1;
    int pix_len = (int)w * (int)h * 2;
    nvs_handle_t hn;
    if (nvs_open(NS, NVS_READWRITE, &hn) != ESP_OK) return -1;
    char key[24]; icon_key((const uint8_t *)rp, rp_len, key);

    uint8_t blob[2 + CK_ICON_MAX_PIX];
    blob[0] = w; blob[1] = h;
    memcpy(blob + 2, pixels, pix_len);
    esp_err_t e = nvs_set_blob(hn, key, blob, 2 + pix_len);
    if (e == ESP_OK) nvs_commit(hn);
    nvs_close(hn);
    return e == ESP_OK ? 0 : -1;
}

int ck_store_icon_get(const char *rp, uint8_t *out, int out_cap, uint8_t *w, uint8_t *h)
{
    nvs_handle_t hn;
    if (nvs_open(NS, NVS_READONLY, &hn) != ESP_OK) return -1;
    char key[24]; icon_key((const uint8_t *)rp, (int)strlen(rp), key);

    uint8_t blob[2 + CK_ICON_MAX_PIX];
    size_t len = sizeof(blob);
    esp_err_t e = nvs_get_blob(hn, key, blob, &len);
    nvs_close(hn);
    if (e != ESP_OK || len < 2) return -1;
    uint8_t bw = blob[0], bh = blob[1];
    if (bw < 1 || bw > 32 || bh < 1 || bh > 32) return -1;
    int pix_len = (int)bw * (int)bh * 2;
    if ((int)len != 2 + pix_len || pix_len > out_cap) return -1;
    memcpy(out, blob + 2, pix_len);
    *w = bw; *h = bh;
    return 0;
}
