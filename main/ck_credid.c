// Stateless credential IDs. See ck_credid.h.
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include <string.h>
#include "ck_credid.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "mbedtls/platform_util.h"
#include "esp_random.h"

// Device master secret. MULE ONLY: a fixed constant, so credentials survive a
// reflash of the same firmware. PRODUCT: derive from eFuse or a
// flash-encrypted NVS blob (docs §10 device_secret provenance is still open).
static const uint8_t CK_MK[32] = {
    0x9d, 0x4b, 0xf2, 0x27, 0x51, 0x0e, 0x8a, 0x63, 0x2c, 0x1f, 0x77, 0xb9,
    0xd5, 0x3e, 0x40, 0x18, 0x66, 0x2b, 0x8c, 0x74, 0xa1, 0x09, 0xe3, 0x55,
    0xf8, 0x12, 0xba, 0x9e, 0x47, 0x6d, 0x30, 0xc1,
};
static const char WRAP_SALT[] = "core-keys/fido2/wrap/v1";
static const char WRAP_INFO[] = "core-keys/fido2/cred-wrap/v1";
// AAD = version byte || domain-separation label (authenticated, not encrypted).
static const uint8_t CRED_AAD[] = {
    0x01, 'c','o','r','e','-','k','e','y','s','/','f','i','d','o','2','/',
    'c','r','e','d','/','v','1',
};

// HKDF-SHA256, single output block (L <= 32). Hand-rolled over HMAC so we do
// not depend on MBEDTLS_HKDF_C being enabled in the sdkconfig.
static int hkdf_sha256(const uint8_t *salt, size_t salt_len,
                       const uint8_t *ikm, size_t ikm_len,
                       const uint8_t *info, size_t info_len,
                       uint8_t *out, size_t out_len)
{
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md || out_len > 32) return -1;

    uint8_t prk[32], t[32];
    int rc = mbedtls_md_hmac(md, salt, salt_len, ikm, ikm_len, prk);  // Extract
    if (rc == 0) {
        mbedtls_md_context_t ctx;
        mbedtls_md_init(&ctx);
        rc = mbedtls_md_setup(&ctx, md, 1);
        if (rc == 0) rc = mbedtls_md_hmac_starts(&ctx, prk, 32);      // Expand, T(1)
        if (rc == 0) rc = mbedtls_md_hmac_update(&ctx, info, info_len);
        if (rc == 0) { uint8_t c = 0x01; rc = mbedtls_md_hmac_update(&ctx, &c, 1); }
        if (rc == 0) rc = mbedtls_md_hmac_finish(&ctx, t);
        mbedtls_md_free(&ctx);
    }
    if (rc == 0) memcpy(out, t, out_len);
    mbedtls_platform_zeroize(prk, sizeof(prk));
    mbedtls_platform_zeroize(t, sizeof(t));
    return rc;
}

static int derive_kwrap(uint8_t k[32])
{
    return hkdf_sha256((const uint8_t *)WRAP_SALT, sizeof(WRAP_SALT) - 1,
                       CK_MK, sizeof(CK_MK),
                       (const uint8_t *)WRAP_INFO, sizeof(WRAP_INFO) - 1, k, 32);
}

void ck_rpid_hash(const uint8_t *rp_id, size_t len, uint8_t out[32])
{
    mbedtls_sha256(rp_id, len, out, 0);
}

int ck_credid_wrap(const uint8_t d[32], const uint8_t rpid_hash[32],
                   uint8_t out[CK_CREDID_LEN])
{
    uint8_t k[32], pt[64], nonce[12];
    int rc = derive_kwrap(k);
    if (rc) return rc;

    memcpy(pt, d, 32);
    memcpy(pt + 32, rpid_hash, 32);
    esp_fill_random(nonce, 12);

    out[0] = 0x01;
    memcpy(out + 1, nonce, 12);

    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, k, 256);
    if (rc == 0)
        rc = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, 64, nonce, 12,
                                       CRED_AAD, sizeof(CRED_AAD),
                                       pt, out + 13, 16, out + 77);
    mbedtls_gcm_free(&g);
    mbedtls_platform_zeroize(k, sizeof(k));
    mbedtls_platform_zeroize(pt, sizeof(pt));
    return rc;
}

int ck_credid_unwrap(const uint8_t *cred_id, size_t len, const uint8_t rpid_hash[32],
                     uint8_t d_out[32])
{
    if (len != CK_CREDID_LEN || cred_id[0] != 0x01) return -1;

    uint8_t k[32], pt[64];
    int rc = derive_kwrap(k);
    if (rc) return rc;

    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, k, 256);
    if (rc == 0)
        rc = mbedtls_gcm_auth_decrypt(&g, 64, cred_id + 1, 12,
                                      CRED_AAD, sizeof(CRED_AAD),
                                      cred_id + 77, 16, cred_id + 13, pt);
    mbedtls_gcm_free(&g);
    mbedtls_platform_zeroize(k, sizeof(k));

    if (rc == 0) {
        if (memcmp(pt + 32, rpid_hash, 32) != 0) rc = -1;   // right key, wrong RP
        else memcpy(d_out, pt, 32);
    }
    mbedtls_platform_zeroize(pt, sizeof(pt));
    return rc;
}
