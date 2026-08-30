// Persistent key store (NVS): the device's own X25519 static identity and the
// list of authorized (pinned) daemon statics. Replaces the fixed bring-up keys —
// the device generates its identity on first boot and pins a daemon only through
// the pairing ceremony (docs §4).
#pragma once
#include <stdint.h>

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
