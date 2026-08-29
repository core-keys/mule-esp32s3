// Stateless, non-discoverable FIDO2 credential IDs.
//
// A credentialId is the wrapped private key: it carries no server-side state, so
// the device holds no per-credential storage. Layout (93 bytes):
//   ver(0x01) || nonce(12) || AES-256-GCM ct(64) || tag(16)
// wrapping plaintext (P-256 scalar d(32) || rpIdHash(32)) under
// K_wrap = HKDF-SHA256(MK). getAssertion unwraps, checks the GCM tag AND that
// the embedded rpIdHash matches the request's RP, so a handle minted for one RP
// cannot be exercised at another. See scratchpad credmodel / docs §5.1.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define CK_CREDID_LEN 93

// rpIdHash = SHA-256(rp_id).
void ck_rpid_hash(const uint8_t *rp_id, size_t len, uint8_t out[32]);

// Wrap (d || rpid_hash) into a 93-byte credentialId. Returns 0 on success.
int ck_credid_wrap(const uint8_t d[32], const uint8_t rpid_hash[32],
                   uint8_t out[CK_CREDID_LEN]);

// Unwrap: verifies length, version, GCM tag, and embedded rpIdHash == rpid_hash.
// On success writes the recovered scalar to d_out and returns 0; any failure
// (not ours / tampered / wrong RP) returns nonzero.
int ck_credid_unwrap(const uint8_t *cred_id, size_t len, const uint8_t rpid_hash[32],
                     uint8_t d_out[32]);
