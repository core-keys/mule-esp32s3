// P-256 / ES256 (mbedTLS) for the FIDO2 credential keys.
//
// makeCredential mints a fresh keypair; getAssertion reloads one from the
// unwrapped 32-byte scalar. Signatures are DER ECDSA over SHA-256(msg) with a
// hedged nonce (TRNG || d || msgHash) and verify-after-sign — mirroring the
// frozen Ed25519 policy (docs/coauth-protocol.md §6).
#pragma once
#include <stdint.h>
#include <stddef.h>

// Generate a P-256 keypair. d = 32-byte big-endian private scalar; x,y = raw
// big-endian affine public coordinates. Returns 0 on success.
int ck_p256_keygen(uint8_t d[32], uint8_t x[32], uint8_t y[32]);

// Sign `msg` (SHA-256 is applied internally) with scalar `d`. Writes a DER
// ECDSA signature into `sig` (cap >= 72) and sets *sig_len. Returns 0 on
// success (includes a verify-after-sign check).
int ck_p256_sign(const uint8_t d[32], const uint8_t *msg, size_t msg_len,
                 uint8_t *sig, size_t cap, size_t *sig_len);
