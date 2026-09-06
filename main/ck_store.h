// Persistent key store (NVS): the device's own X25519 static identity and the
// list of authorized (pinned) daemon statics. Replaces the fixed bring-up keys —
// the device generates its identity on first boot and pins a daemon only through
// the pairing ceremony (docs §4).
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define CK_MAX_AUTH_DAEMONS 8

// Bring up NVS. Call once at boot before any other ck_store_* call.
void ck_store_init(void);

// Load the device X25519 static, generating + persisting it on first boot.
// Returns 0 on success.
int ck_store_device_static(uint8_t priv[32], uint8_t pub[32]);

// Authorized daemon list.
int ck_store_auth_count(void);                  // number of pinned daemons
int ck_store_auth_get(int idx, uint8_t pub[32]); // 0 on success
int ck_store_auth_add(const uint8_t pub[32]);    // pin (append, dedup); 0 on success
int ck_store_auth_clear(void);                   // revoke all (factory reset)

// ---- Device unlock PIN (local, not CTAP2 clientPIN) --------------------------
#define CK_PIN_MAX_RETRIES 8
bool ck_store_has_pin(void);
int  ck_store_pin_set(const char *pin);          // salted-hash + store; resets retries
// 0 = correct (retries reset), 1 = wrong, -1 = locked out (retries exhausted).
int  ck_store_pin_verify(const char *pin);
int  ck_store_pin_retries_left(void);
int  ck_store_pin_clear(void);                   // remove the PIN (factory reset)

// ---- Credential journal (display-only; keys stay stateless) ------------------
#define CK_CRED_MAX 8
int  ck_store_cred_count(void);
// Append/update by rp (one entry per site); fp = SHA-256(credId)[0..4].
int  ck_store_cred_add(const char *rp, const char *user, const uint8_t *cred_id, int cred_len);
int  ck_store_cred_get(int idx, char *rp, int rp_cap, char *user, int user_cap);
// Remove one journal entry by index, compacting the list. Display-only: the
// stateless credential itself is unaffected (there is nothing else to erase).
int  ck_store_cred_del(int idx);

// ---- Per-site icon store (favicons; keyed by rp hash, not credential slot) ---
// Store one site icon (overwrites any existing icon for this rp). Pixels are
// RGB565 little-endian, w*h*2 bytes, row-major. Returns 0 on success.
int ck_store_icon_set(const char *rp, int rp_len, uint8_t w, uint8_t h, const uint8_t *pixels);
// Fetch the icon for rp into out (pixels, w*h*2 bytes). Returns 0 and fills
// *w,*h on success; -1 if none / doesn't fit out_cap.
int ck_store_icon_get(const char *rp, uint8_t *out, int out_cap, uint8_t *w, uint8_t *h);
