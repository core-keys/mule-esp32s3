#include "crypto/ed25519/ed25519.h"
#include "ck_ed25519.h"

void ck_ed25519_pubkey(const uint8_t priv_seed[32], uint8_t pub_out[32])
{
    ed25519_publickey(priv_seed, pub_out);
}

void ck_ed25519_sign(const uint8_t *msg, size_t msg_len,
                     const uint8_t priv_seed[32], const uint8_t pub[32],
                     uint8_t sig_out[64])
{
    ed25519_sign(msg, msg_len, priv_seed, pub, sig_out);
}
