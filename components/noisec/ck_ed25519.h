// Minimal Ed25519 signing for the device SSH credential key, backed by noise-c's
// bundled ed25519-donna (RFC 8032). The 32-byte private key is a seed.
#pragma once
#include <stdint.h>
#include <stddef.h>

void ck_ed25519_pubkey(const uint8_t priv_seed[32], uint8_t pub_out[32]);
void ck_ed25519_sign(const uint8_t *msg, size_t msg_len,
                     const uint8_t priv_seed[32], const uint8_t pub[32],
                     uint8_t sig_out[64]);
