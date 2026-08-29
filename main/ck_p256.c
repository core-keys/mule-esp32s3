// P-256 / ES256 via mbedTLS. See ck_p256.h.
#define MBEDTLS_ALLOW_PRIVATE_ACCESS   // read kp.d / kp.grp / kp.Q directly (3.x)
#include <string.h>
#include "ck_p256.h"
#include "mbedtls/ecp.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/sha256.h"
#include "mbedtls/platform_util.h"
#include "esp_random.h"

// f_rng adapter over the ESP32-S3 hardware RNG (esp_fill_random).
static int esp_rng(void *ctx, unsigned char *buf, size_t len)
{
    (void)ctx;
    esp_fill_random(buf, len);
    return 0;
}

int ck_p256_keygen(uint8_t d[32], uint8_t x[32], uint8_t y[32])
{
    mbedtls_ecp_keypair kp;
    mbedtls_ecp_keypair_init(&kp);

    int rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, &kp, esp_rng, NULL);
    if (rc == 0) rc = mbedtls_mpi_write_binary(&kp.d, d, 32);
    if (rc == 0) {
        uint8_t pt[65];
        size_t olen = 0;
        rc = mbedtls_ecp_point_write_binary(&kp.grp, &kp.Q, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                            &olen, pt, sizeof(pt));
        if (rc == 0 && olen == 65) {          // 0x04 || X[32] || Y[32]
            memcpy(x, pt + 1, 32);
            memcpy(y, pt + 33, 32);
        } else if (rc == 0) {
            rc = -1;
        }
    }
    mbedtls_ecp_keypair_free(&kp);
    return rc;
}

int ck_p256_sign(const uint8_t d[32], const uint8_t *msg, size_t msg_len,
                 uint8_t *sig, size_t cap, size_t *sig_len)
{
    uint8_t digest[32];
    mbedtls_ecp_keypair kp;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ecp_keypair_init(&kp);
    mbedtls_ctr_drbg_init(&drbg);

    int rc = mbedtls_ecp_read_key(MBEDTLS_ECP_DP_SECP256R1, &kp, d, 32);
    if (rc == 0) rc = mbedtls_ecp_keypair_calc_public(&kp, esp_rng, NULL);
    if (rc == 0) rc = mbedtls_sha256(msg, msg_len, digest, 0);

    if (rc == 0) {
        // Hedged nonce: personalize a CTR_DRBG with TRNG || d || SHA-256(msg).
        uint8_t pers[32 + 32 + 32];
        esp_fill_random(pers, 32);
        memcpy(pers + 32, d, 32);
        memcpy(pers + 64, digest, 32);
        rc = mbedtls_ctr_drbg_seed(&drbg, esp_rng, NULL, pers, sizeof(pers));
        mbedtls_platform_zeroize(pers, sizeof(pers));
    }
    if (rc == 0)
        rc = mbedtls_ecdsa_write_signature(&kp, MBEDTLS_MD_SHA256, digest, 32,
                                           sig, cap, sig_len,
                                           mbedtls_ctr_drbg_random, &drbg);
    // Verify-after-sign against the just-derived public key (fault detection).
    if (rc == 0)
        rc = mbedtls_ecdsa_read_signature(&kp, digest, 32, sig, *sig_len);

    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_ecp_keypair_free(&kp);
    mbedtls_platform_zeroize(digest, sizeof(digest));
    return rc;
}
